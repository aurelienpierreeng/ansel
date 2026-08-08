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


#include "gui/bauhaus_conf.h"

#include "common/conf.h"
#include "common/utility.h"
#include "system/mem_alloc.h"

#include <glib/gi18n.h>
#include <stdio.h>

static void _combobox_conf_value_changed(GtkWidget *widget, gpointer user_data)
{
  const char *value = (const char *)dt_bauhaus_combobox_get_data(widget);
  if(value) dt_conf_set_string((const char *)user_data, value);
}

GtkWidget *dt_bauhaus_combobox_from_conf(dt_bauhaus_t *bh, dt_gui_module_t *self, const char *confkey)
{
  if(dt_confgen_type(confkey) != DT_ENUM || !dt_confgen_value_exists(confkey, DT_VALUES))
  {
    fprintf(stderr, "[dt_bauhaus_combobox_from_conf] `%s` is not declared as an <enum> config entry\n", confkey);
    return NULL;
  }

  GtkWidget *combo = dt_bauhaus_combobox_new(bh, self);
  dt_bauhaus_widget_set_label(combo, _(dt_confgen_get_label(confkey)));

  const char *tooltip = dt_confgen_get_tooltip(confkey);
  gtk_widget_set_tooltip_text(combo, (tooltip && *tooltip) ? _(tooltip) : _(dt_confgen_get_label(confkey)));

  gchar *current = dt_conf_get_string(confkey);
  const char *values = dt_confgen_get(confkey, DT_VALUES);
  GList *options = dt_util_str_to_glist("][", values);

  int pos = 0, active = 0;
  for(GList *opt = options; opt; opt = g_list_next(opt))
  {
    char *item = (char *)opt->data;
    // strip the leading '[' of the first entry and the trailing ']' of the last one
    if(item[0] == '[') item++;
    else if(item[strlen(item) - 1] == ']') item[strlen(item) - 1] = '\0';

    dt_bauhaus_combobox_add_full(combo, g_dpgettext2(NULL, "preferences", item), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                  g_strdup(item), dt_free_gpointer, TRUE);

    if(!g_strcmp0(current, item)) active = pos;
    pos++;
  }
  g_list_free_full(options, dt_free_gpointer);
  dt_free(current);

  // Select the entry matching the current config value before connecting the signal,
  // so the initial sync doesn't bounce back through dt_conf_set_string().
  dt_bauhaus_combobox_set(combo, active);

  g_signal_connect(G_OBJECT(combo), "value-changed", G_CALLBACK(_combobox_conf_value_changed), (gpointer)confkey);

  return combo;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
