/*
 *    This file is part of darktable,
 *    Copyright (C) 2017-2021 darktable developers.
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

#include <stdlib.h>
#include <string.h>
#include <gmodule.h>
#include <glib/gi18n.h>

#include "config.h"
#include "common/file_location.h"
#include "common/module.h"
#include "control/conf.h"
#include "gui/splash.h"

GList *dt_module_load_modules(const char *subdir, size_t module_size,
                              int (*load_module_so)(void *module, const char *libname, const char *plugin_name),
                              void (*init_module)(void *module),
                              gint (*sort_modules)(gconstpointer a, gconstpointer b))
{
  GList *plugin_list = NULL;
  char moduledir[PATH_MAX] = { 0 };
  const gchar *dir_name;
  dt_loc_get_moduledir(moduledir, sizeof(moduledir));
  g_strlcat(moduledir, subdir, sizeof(moduledir));
  GDir *dir = g_dir_open(moduledir, 0, NULL);
  if(!dir) return NULL;
  const int name_offset = strlen(SHARED_MODULE_PREFIX),
            name_end = strlen(SHARED_MODULE_PREFIX) + strlen(SHARED_MODULE_SUFFIX);
  while((dir_name = g_dir_read_name(dir)))
  {
    // get lib*.so
    if(!g_str_has_prefix(dir_name, SHARED_MODULE_PREFIX)) continue;
    if(!g_str_has_suffix(dir_name, SHARED_MODULE_SUFFIX)) continue;
    char *plugin_name = g_strndup(dir_name + name_offset, strlen(dir_name) - name_end);
    void *module = calloc(1, module_size);
    gchar *libname = g_module_build_path(moduledir, plugin_name);

    int res = 1;

    // Get the preference to enable/disable the plugin.
    gchar *pref_line = g_strdup_printf("%s/%s/enable", subdir, plugin_name);
    int load;

    if(dt_conf_key_exists(pref_line))
    {
      // Disable plugins only if we have an explicit rule saying so.
      load = dt_conf_get_bool(pref_line);
      // fprintf(stdout, "%s exists : %i\n", pref_line, load);
    }
    else
    {
      // If no rule, then enable by default.
      load = TRUE;
      dt_conf_set_bool(pref_line, TRUE);
      // fprintf(stdout, "%s does NOT exist\n", pref_line);
    }

    g_free(pref_line);

    dt_gui_splash_updatef(_("Loading module %s..."), plugin_name);

    if(load) res = load_module_so(module, libname, plugin_name);
    // if(res) fprintf(stdout, "Plugin %s/%s NOT loaded\n", subdir, plugin_name);

    g_free(plugin_name);
    g_free(libname);

    if(res)
    {
      free(module);
      continue;
    }

    plugin_list = g_list_prepend(plugin_list, module);

    if(init_module) init_module(module);
  }
  g_dir_close(dir);

  if(sort_modules)
    plugin_list = g_list_sort(plugin_list, sort_modules);
  else
    plugin_list = g_list_reverse(plugin_list);  // list was built in reverse order, so un-reverse it

 return plugin_list;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
