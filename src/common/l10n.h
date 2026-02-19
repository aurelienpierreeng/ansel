/*
 *    This file is part of darktable,
 *    Copyright (C) 2016 johannes hanika.
 *    Copyright (C) 2016, 2018 Tobias Ellinghaus.
 *    Copyright (C) 2020 Pascal Obry.
 *    Copyright (C) 2022 Martin Bařinka.
 *    Copyright (C) 2025 Aurélien PIERRE.
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

#pragma once

#include <glib.h>

typedef struct dt_l10n_language_t
{
  char *code, *base_code;
  char *name;
  gboolean is_default;
} dt_l10n_language_t;

typedef struct dt_l10n_t
{
  GList *languages;
  int selected;
  int sys_default;
} dt_l10n_t;

dt_l10n_t *dt_l10n_init(gboolean init_list);
void dt_l10n_disable_setlocale_early(void);

const char *dt_l10n_get_current_lang(dt_l10n_t *l10n);

const char *dt_l10n_get_name(const dt_l10n_language_t *language);
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
