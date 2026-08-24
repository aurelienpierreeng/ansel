/*
 *    This file is part of darktable,
 *    Copyright (C) 2017 Tobias Ellinghaus.
 *    Copyright (C) 2020-2021 Pascal Obry.
 *    Copyright (C) 2021 Ralf Brown.
 *    Copyright (C) 2022 Martin Bařinka.
 *    Copyright (C) 2023 Alynx Zhou.
 *    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
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

#include "system/macros.h"
#include "system/mem_alloc.h"
#include <stdlib.h>
#include <string.h>
#include <gmodule.h>
#include <glib/gi18n.h>

#include "config.h"
#include "common/file_location.h"
#include "common/logging.h"
#include "common/module.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "common/conf.h"

/**
 * @brief Is this manifest entry a plain module base name?
 *
 * A manifest names the modules shipped in its own directory. Anything that could
 * address a different one -- a separator, a `..', a leading dot -- is refused rather
 * than sanitised, because a manifest that says something we do not understand is a
 * manifest we should not be acting on at all.
 */
static gboolean _manifest_entry_is_sane(const char *entry)
{
  if(IS_NULL_PTR(entry) || *entry == '\0') return FALSE;
  if(*entry == '.') return FALSE;
  if(!IS_NULL_PTR(strchr(entry, '/'))) return FALSE;
  if(!IS_NULL_PTR(strchr(entry, '\\'))) return FALSE;
  if(!IS_NULL_PTR(strstr(entry, ".."))) return FALSE;
  return TRUE;
}

gchar **dt_module_read_manifest(const char *subdir, char *moduledir)
{
  moduledir[0] = '\0';
  dt_loc_get_moduledir(moduledir, DT_PATH_MAX);
  g_strlcat(moduledir, subdir, DT_PATH_MAX);

  gchar *manifest_path = g_build_filename(moduledir, DT_MODULE_MANIFEST_NAME, NULL);
  gchar *contents = NULL;
  GError *error = NULL;

  if(!g_file_get_contents(manifest_path, &contents, NULL, &error))
  {
    /* No manifest, no modules. This used to be a g_dir_open() scan that loaded every
     * shared object it found, which meant an install directory a failed upgrade had
     * left in a mixed state could hand us a module built against a struct layout that
     * has since moved -- accepted, because DT_MODULE_VERSION had not changed, and then
     * writing through offsets that no longer exist. Refusing to load beats guessing. */
    dt_print(DT_DEBUG_ALWAYS, "[dt_module_load_modules] no module manifest at `%s': %s\n"
                              "  No module will be loaded from that directory. This is an\n"
                              "  incomplete or damaged installation -- reinstall Ansel.\n",
             manifest_path, error ? error->message : "unknown error");
    if(error) g_error_free(error);
    g_free(manifest_path);
    return NULL;
  }

  gchar **lines = g_strsplit(contents, "\n", -1);
  g_free(contents);

  GPtrArray *names = g_ptr_array_new();
  for(gchar **line = lines; !IS_NULL_PTR(*line); line++)
  {
    gchar *entry = g_strstrip(*line);
    if(*entry == '\0' || *entry == '#') continue;

    if(!_manifest_entry_is_sane(entry))
    {
      dt_print(DT_DEBUG_ALWAYS, "[dt_module_load_modules] `%s' lists `%s', which is not a module name. Ignored.\n",
               manifest_path, entry);
      continue;
    }
    g_ptr_array_add(names, g_strdup(entry));
  }
  g_ptr_array_add(names, NULL);
  g_strfreev(lines);
  g_free(manifest_path);

  return (gchar **)g_ptr_array_free(names, FALSE);
}

GList *dt_module_load_modules(const char *subdir, size_t module_size,
                              int (*load_module_so)(void *module, const char *libname, const char *plugin_name),
                              void (*init_module)(void *module),
                              gint (*sort_modules)(gconstpointer a, gconstpointer b))
{
  GList *plugin_list = NULL;
  char moduledir[DT_PATH_MAX] = { 0 };

  gchar **plugin_names = dt_module_read_manifest(subdir, moduledir);
  if(IS_NULL_PTR(plugin_names)) return NULL;

  for(gchar **name = plugin_names; !IS_NULL_PTR(*name); name++)
  {
    const char *plugin_name = *name;
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
    }
    else
    {
      // If no rule, then enable by default.
      load = TRUE;
      dt_conf_set_bool(pref_line, TRUE);
    }

    dt_free(pref_line);

    if(load) res = load_module_so(module, libname, plugin_name);

    dt_free(libname);

    if(res)
    {
      dt_free(module);
      continue;
    }

    plugin_list = g_list_prepend(plugin_list, module);

    if(init_module) init_module(module);
  }

  g_strfreev(plugin_names);

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
