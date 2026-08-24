/*
    This file is part of darktable,
    Copyright (C) 2017 Tobias Ellinghaus.
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

#ifndef DT_COMMON_MODULE_H
#define DT_COMMON_MODULE_H

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Name of the manifest each module directory must carry.
 *
 * It is generated at build time from the very list of targets CMake compiled, and
 * installed beside them. See dt_module_read_manifest().
 */
#define DT_MODULE_MANIFEST_NAME "modules.manifest"

/**
 * @brief Read a module directory's manifest and return the module base names it lists.
 *
 * @param subdir directory under the module dir, e.g. "/views", leading separator included
 * @param moduledir filled with the absolute directory the manifest was read from;
 *        must be at least DT_PATH_MAX bytes
 * @return a NULL-terminated array of base names, to be freed with g_strfreev(), or NULL
 *         when the manifest is absent or unreadable -- in which case nothing loads.
 *
 * The base names are what g_module_build_path() expects: no `lib' prefix, no file
 * extension, no directory component. Entries carrying a path separator, a `..', or a
 * leading dot are rejected and reported: a manifest names the modules shipped in ITS
 * directory and nothing else.
 */
gchar **dt_module_read_manifest(const char *subdir, char *moduledir);

/**
 * @brief Load every module the given directory's manifest lists.
 *
 * @see dt_module_read_manifest() for how the set of modules is decided.
 */
GList *dt_module_load_modules(const char *subdir, size_t module_size,
                              int (*load_module_so)(void *module, const char *libname, const char *plugin_name),
                              void (*init_module)(void *module),
                              gint (*sort_modules)(gconstpointer a, gconstpointer b));

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_MODULE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
