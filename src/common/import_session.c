/*
    This file is part of darktable,
    Copyright (C) 2014-2021 darktable developers.

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

#include <stdio.h>

#include "common/film.h"
#include "common/import_session.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/datetime.h"
#include "common/utility.h"

/* TODO: Investigate if we can make one import session instance thread safe
         eg. having several background jobs working with same instance.
*/

typedef struct dt_import_session_t
{
  gchar *dest_subfolder;
  gchar *dest_filename;

  uint32_t ref;

  dt_film_t *film;
  dt_variables_params_t *vp;

  gchar *current_path;
  gchar *current_filename;

} dt_import_session_t;

static void _import_session_migrate_old_config()
{
  /* TODO: check if old config exists, migrate to new and remove old */
}

struct dt_import_session_t *dt_import_session_new()
{
  dt_import_session_t *is;

  is = (dt_import_session_t *)g_malloc0(sizeof(dt_import_session_t));

  dt_variables_params_init(&is->vp);

  /* migrate old configuration */
  _import_session_migrate_old_config();
  return is;
}

void dt_import_session_set_name(struct dt_import_session_t *self, const char *name)
{
  /* free previous jobcode name */
  g_free((void *)self->vp->jobcode);

  self->vp->jobcode = g_strdup(name);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
