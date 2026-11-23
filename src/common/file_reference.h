/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

/** @file common/file_reference.h
 *
 * @brief Finding again a file the user chose for an image, once the tree it lives in has moved.
 *
 * A file chosen for an image -- the flat field reference of a raw, its dark frame -- is kept as three
 * things: its absolute path, its path relative to the image's folder, and what it is: its size and
 * the SHA-256 of its bytes. The relative path survives moving the whole tree, to another disk or to
 * another computer, Linux or Windows; the absolute path survives moving the image alone; and what the
 * file is lets it be found by its name once it moved by itself, without taking another file of the
 * same name for it.
 */

#ifndef DT_COMMON_FILE_REFERENCE_H
#define DT_COMMON_FILE_REFERENCE_H

#include <glib.h>
#include <stdint.h>

/** How many folders above the image's are searched by name, each with the folders directly in it. */
#define DT_FILE_REFERENCE_PARENTS 3

/** Bytes of a SHA-256. */
#define DT_FILE_REFERENCE_SHA256_SIZE 32

typedef enum dt_file_reference_status_t
{
  DT_FILE_REFERENCE_FOUND = 0, //!< at its relative path, at its absolute path, or by its name and what it is
  DT_FILE_REFERENCE_OTHERS,    //!< not found, but files of its name were: none of them is that file
  DT_FILE_REFERENCE_MISSING,   //!< not found, and no file of its name either
} dt_file_reference_status_t;

/** A file the user chose, as it was stored. The strings are the caller's. */
typedef struct dt_file_reference_t
{
  const char *path;      //!< absolute, as chosen, with the separators of the system it was chosen on
  const char *relative;  //!< from the image's folder, '/'-separated; "" when there is none
  uint32_t size;         //!< in bytes; UINT32_MAX for a file of 4 GiB or more
  const uint8_t *sha256; //!< the DT_FILE_REFERENCE_SHA256_SIZE bytes of the SHA-256 of its content
} dt_file_reference_t;

/** @brief The path of @p path seen from the directory @p folder, so that it means the same on Linux and
 * on Windows: '/'-separated, with ".." for each level up.
 *
 * Both paths are absolute. On Windows, names are compared ignoring the case of ASCII letters, as its
 * file systems do.
 *
 * @return a newly allocated path, or NULL when there is none: on Windows, another drive or another
 *         network share */
gchar *dt_file_reference_relative(const char *folder, const char *path);

/** @brief The name of the file at @p path, after its last separator -- '/' or '\\', whatever the system,
 * since the path may have been chosen on the other one. Points into @p path. */
const char *dt_file_reference_name(const char *path);

/** @brief What the file at @p path is: its size, and the SHA-256 of all its bytes.
 *
 * The file is read whole: about 0.1 s for a 23 MB raw.
 *
 * @return 0, or 1 when it cannot be read, leaving @p size and @p sha256 as they were */
int dt_file_reference_identify(const char *path, uint32_t *size, uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE]);

/** @brief Where the file of @p ref is now, for an image in @p folder.
 *
 * It is looked for at its relative path from @p folder first, then at its absolute path. A file there
 * is taken as it is, even if it changed since it was chosen: a reference replaced in place is used.
 * Then it is looked for by its name, ignoring the case of ASCII letters: in @p folder and in the
 * DT_FILE_REFERENCE_PARENTS folders above it, each with the folders directly in it but hidden ones,
 * the nearest first. A file found by its name is taken only if it has the size and the SHA-256 of
 * @p ref. The size, which costs nothing, is compared first, so that a file is read whole only when it
 * could be that one.
 *
 * What the search by name finds is remembered for the session: asked again for the same file from the
 * same folder, it answers without listing a folder again, unless the file it found has gone since. Any
 * thread.
 *
 * @param ref    the file; its path is not empty
 * @param folder the absolute path of the image's folder
 * @param found  receives where the file is now, newly allocated, when it is found; NULL otherwise
 * @param others receives the files of its name, nearest first, when files of its name were found but
 *               not that one: newly allocated paths in a list the caller frees with
 *               g_list_free_full(…, g_free). NULL when not wanted.
 * @return whether the file was found */
dt_file_reference_status_t dt_file_reference_find(const dt_file_reference_t *ref, const char *folder,
                                                  gchar **found, GList **others);

#endif // DT_COMMON_FILE_REFERENCE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
