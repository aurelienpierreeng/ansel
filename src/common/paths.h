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

/**
 * @brief Buffer size for a filesystem path anywhere in Ansel.
 *
 * This is deliberately NOT the system PATH_MAX. That macro is not a constant across the
 * platforms we ship: 4096 on Linux, 1024 on macOS, and 260 on Windows -- mingw's
 * <limits.h> defines it unconditionally, and redefines it to 512 under _POSIX_, so it
 * even overrides a value we set first.
 *
 * What used to live here was `#ifndef PATH_MAX / #define PATH_MAX 4096`, which is dead
 * code on all three platforms, because all three define it. The visible consequence was
 * that every `char buf[PATH_MAX]` in the tree -- including the five path members of
 * dt_image_t -- was a 4096-byte buffer when built on Linux and a 260-byte one when built
 * on Windows, where paths under %LOCALAPPDATA% get close to that and were silently
 * truncated by the g_strlcpy/snprintf that fill them.
 *
 * Sizing our own buffers is our decision, so we make it once, here, for every platform.
 * Note this does not by itself let Windows OPEN a path longer than its own MAX_PATH --
 * that needs a long-path-aware manifest -- it only stops us truncating before the OS is
 * ever asked.
 */
#define DT_PATH_MAX 4096

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
void dt_concat_path_file(char destination[DT_PATH_MAX], const char path[DT_PATH_MAX], const char *const file);

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_PATHS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
