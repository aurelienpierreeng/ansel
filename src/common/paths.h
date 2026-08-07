/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef DT_COMMON_PATHS_H
#define DT_COMMON_PATHS_H

/* g_open() takes O_BINARY on Windows and does not define it elsewhere. Kept here rather
 * than in darktable.h so that opening a file does not require the application. */
#if !defined(O_BINARY)
#define O_BINARY 0
#endif

/* Path/filename length constants and path splicing. Application-free on purpose:
 * include this instead of darktable.h. */

#ifdef __cplusplus
extern "C" {
#endif

/** define for max path/filename length */
#define DT_MAX_FILENAME_LEN 256

#ifndef PATH_MAX
/*
 * from /usr/include/linux/limits.h (Linux 3.16.5)
 * Some systems might not define it (e.g. Hurd)
 *
 * We do NOT depend on any specific value of this env variable.
 * If you want constant value across all systems, use DT_MAX_PATH_FOR_PARAMS!
 */
#define PATH_MAX 4096
#endif

/*
 * ONLY TO BE USED FOR PARAMS!!! (e.g. dt_imageio_disk_t)
 *
 * WARNING: this should *NEVER* be changed, as it will break params,
 *          created with previous DT_MAX_PATH_FOR_PARAMS.
 */
#define DT_MAX_PATH_FOR_PARAMS 4096

/**
 * @brief Append a constant filename to a variable, stack-based, fixed-sized, directory, 
 * and add a `/` in-between
 * 
 * @param destination 
 * @param variable 
 * @param string 
 */
void dt_concat_path_file(char destination[PATH_MAX], const char path[PATH_MAX], const char *const file);

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_PATHS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
