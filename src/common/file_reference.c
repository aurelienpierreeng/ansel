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

#include "common/file_reference.h"

#include "system/macros.h"    // IS_NULL_PTR
#include "system/mem_alloc.h" // dt_free

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Windows takes both separators, and compares names ignoring case
#ifdef _WIN32
#define DT_FILE_REFERENCE_SEPARATORS "\\/"
#define DT_FILE_REFERENCE_SAME_NAME(a, b) (!g_ascii_strcasecmp((a), (b)))
#else
#define DT_FILE_REFERENCE_SEPARATORS "/"
#define DT_FILE_REFERENCE_SAME_NAME(a, b) (!g_strcmp0((a), (b)))
#endif

/** What a search by name found, kept for the session. */
typedef struct dt_file_reference_memo_t
{
  gchar *found;  // the file, NULL when it was not found
  GList *others; // the files of its name that are not it, nearest first
} dt_file_reference_memo_t;

static GMutex _memo_lock;
static GHashTable *_memo = NULL; // the folder, the name, the size and the SHA-256 -> dt_file_reference_memo_t

static void _memo_free(gpointer data)
{
  dt_file_reference_memo_t *memo = (dt_file_reference_memo_t *)data;
  dt_free(memo->found);
  g_list_free_full(memo->others, g_free);
  dt_free(memo);
}

/* The names of @p path between its separators, without the empty ones a root or a doubled separator
 * leaves. */
static GPtrArray *_components(const char *path)
{
  gchar **parts = g_strsplit_set(path, DT_FILE_REFERENCE_SEPARATORS, -1);
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  for(gchar **part = parts; *part; part++)
    if((*part)[0]) g_ptr_array_add(names, g_strdup(*part));
  g_strfreev(parts);
  return names;
}

gchar *dt_file_reference_relative(const char *folder, const char *path)
{
  GPtrArray *from = _components(folder);
  GPtrArray *to = _components(path);

#ifdef _WIN32
  // the root must be shared: a drive, or for a network path its server and its share
  const guint root = G_IS_DIR_SEPARATOR(path[0]) && G_IS_DIR_SEPARATOR(path[1]) ? 2 : 1;
#else
  const guint root = 0;
#endif

  // the folders both paths go through, the file's own name aside
  guint common = 0;
  while(common < from->len && common + 1 < to->len
        && DT_FILE_REFERENCE_SAME_NAME((const char *)from->pdata[common], (const char *)to->pdata[common]))
    common++;

  gchar *relative = NULL;
  if(common >= root)
  {
    // up from the folder to what both share, then down to the file
    GString *built = g_string_new(NULL);
    for(guint k = common; k < from->len; k++) g_string_append(built, "../");
    for(guint k = common; k < to->len; k++)
    {
      g_string_append(built, (const char *)to->pdata[k]);
      if(k + 1 < to->len) g_string_append_c(built, '/');
    }
    relative = g_string_free(built, FALSE);
  }

  g_ptr_array_free(from, TRUE);
  g_ptr_array_free(to, TRUE);
  return relative;
}

const char *dt_file_reference_name(const char *path)
{
  const char *name = path;
  for(const char *c = path; *c; c++)
    if(*c == '/' || *c == '\\') name = c + 1;
  return name;
}

int dt_file_reference_identify(const char *path, uint32_t *size, uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE])
{
  FILE *file = g_fopen(path, "rb");
  if(IS_NULL_PTR(file)) return 1;

  // a mebibyte at a time: a raw is tens of them, and none of it has to stay in memory
  const size_t chunk_size = 1 << 20;
  guchar *chunk = g_malloc(chunk_size);
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  uint64_t total = 0;
  size_t got = 0;
  while((got = fread(chunk, 1, chunk_size, file)) > 0)
  {
    g_checksum_update(checksum, chunk, got);
    total += got;
  }
  const int failed = ferror(file) ? 1 : 0;
  fclose(file);
  dt_free(chunk);

  if(!failed)
  {
    gsize length = DT_FILE_REFERENCE_SHA256_SIZE;
    g_checksum_get_digest(checksum, sha256, &length);
    *size = (uint32_t)MIN(total, (uint64_t)UINT32_MAX);
  }
  g_checksum_free(checksum);
  return failed;
}

/* Paths in one order, whatever order a file system lists them in. */
static gint _compare_paths(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(*(const char *const *)a, *(const char *const *)b);
}

/* Append to @p files the files directly in @p dir named @p name, ignoring the case of ASCII letters, and
 * to @p folders, unless NULL, the folders directly in it but the hidden ones and @p skip. A folder that
 * cannot be read adds nothing: it cannot be where the file is for us either. */
static void _list(const char *dir, const char *name, const char *skip, GPtrArray *files, GPtrArray *folders)
{
  GFile *parent = g_file_new_for_path(dir);
  GFileEnumerator *entries = g_file_enumerate_children(parent,
                                                       G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
                                                       "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
                                                       G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_object_unref(parent);
  if(IS_NULL_PTR(entries)) return;

  GFileInfo *info = NULL;
  while(!IS_NULL_PTR(info = g_file_enumerator_next_file(entries, NULL, NULL)))
  {
    const char *entry = g_file_info_get_name(info);
    const GFileType type = g_file_info_get_file_type(info);
    gchar *child = g_build_filename(dir, entry, NULL);
    if(type == G_FILE_TYPE_REGULAR && !g_ascii_strcasecmp(entry, name))
      g_ptr_array_add(files, child);
    else if(!IS_NULL_PTR(folders) && type == G_FILE_TYPE_DIRECTORY && !g_file_info_get_is_hidden(info)
            && g_strcmp0(child, skip))
      g_ptr_array_add(folders, child);
    else
      dt_free(child);
    g_object_unref(info);
  }
  g_object_unref(entries);
}

/* Look for the file of @p ref by its @p name around @p folder, the nearest first, as
 * dt_file_reference_find() says. */
static dt_file_reference_memo_t *_search(const dt_file_reference_t *ref, const char *folder, const char *name)
{
  GPtrArray *files = g_ptr_array_new_with_free_func(g_free);

  // each level is a folder and the folders directly in it, but the one the level below searched already
  gchar *dir = g_canonicalize_filename(folder, NULL);
  gchar *below = NULL;
  for(int level = 0; level <= DT_FILE_REFERENCE_PARENTS; level++)
  {
    GPtrArray *folders = g_ptr_array_new_with_free_func(g_free);
    _list(dir, name, below, files, folders);
    g_ptr_array_sort(folders, _compare_paths);
    for(guint k = 0; k < folders->len; k++) _list((const char *)folders->pdata[k], name, NULL, files, NULL);
    g_ptr_array_free(folders, TRUE);

    dt_free(below);
    below = dir;
    gchar *parent = g_path_get_dirname(below);
    dir = g_canonicalize_filename(parent, NULL);
    dt_free(parent);
    if(!g_strcmp0(dir, below)) break; // the root has no parent
  }
  dt_free(dir);
  dt_free(below);

  // what the file is decides between them: its size first, which costs a stat, its content only then
  dt_file_reference_memo_t *memo = g_new0(dt_file_reference_memo_t, 1);
  for(guint k = 0; k < files->len && IS_NULL_PTR(memo->found); k++)
  {
    const char *file = (const char *)files->pdata[k];
    GStatBuf st;
    uint32_t size = 0;
    uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE];
    if(g_stat(file, &st) == 0 && (uint32_t)MIN((uint64_t)st.st_size, (uint64_t)UINT32_MAX) == ref->size
       && !dt_file_reference_identify(file, &size, sha256) && !memcmp(sha256, ref->sha256, sizeof(sha256)))
      memo->found = g_strdup(file);
  }
  for(guint k = files->len; IS_NULL_PTR(memo->found) && k > 0; k--)
    memo->others = g_list_prepend(memo->others, g_strdup((const char *)files->pdata[k - 1]));

  g_ptr_array_free(files, TRUE);
  return memo;
}

dt_file_reference_status_t dt_file_reference_find(const dt_file_reference_t *ref, const char *folder,
                                                  gchar **found, GList **others)
{
  *found = NULL;
  if(!IS_NULL_PTR(others)) *others = NULL;

  // where it was chosen, seen from wherever the image is now: what survives moving the whole tree
  gchar *path = ref->relative[0] ? g_canonicalize_filename(ref->relative, folder) : NULL;
  if(IS_NULL_PTR(path) || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
  {
    // where it was chosen, on this computer: what survives moving the image alone
    dt_free(path);
    path = g_file_test(ref->path, G_FILE_TEST_IS_REGULAR) ? g_strdup(ref->path) : NULL;
  }
  if(!IS_NULL_PTR(path))
  {
    *found = path;
    return DT_FILE_REFERENCE_FOUND;
  }

  // by its name, around the image: that lists folders, so what it finds is kept
  const char *name = dt_file_reference_name(ref->path);
  GString *key = g_string_new(NULL);
  g_string_append_printf(key, "%s\n%s\n%" PRIu32 "\n", folder, name, ref->size);
  for(int k = 0; k < DT_FILE_REFERENCE_SHA256_SIZE; k++) g_string_append_printf(key, "%02x", ref->sha256[k]);
  gchar *memo_key = g_string_free(key, FALSE);

  g_mutex_lock(&_memo_lock);
  if(IS_NULL_PTR(_memo)) _memo = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _memo_free);
  dt_file_reference_memo_t *memo = (dt_file_reference_memo_t *)g_hash_table_lookup(_memo, memo_key);
  // a file found before and gone since is looked for again
  if(IS_NULL_PTR(memo) || (!IS_NULL_PTR(memo->found) && !g_file_test(memo->found, G_FILE_TEST_IS_REGULAR)))
  {
    memo = _search(ref, folder, name);
    g_hash_table_replace(_memo, memo_key, memo);
  }
  else
    dt_free(memo_key);

  *found = g_strdup(memo->found);
  // a copy for the caller, built from the end so that it keeps the order
  for(GList *other = g_list_last(memo->others); !IS_NULL_PTR(others) && !IS_NULL_PTR(other);
      other = g_list_previous(other))
    *others = g_list_prepend(*others, g_strdup((const char *)other->data));
  const dt_file_reference_status_t status = !IS_NULL_PTR(memo->found)    ? DT_FILE_REFERENCE_FOUND
                                            : !IS_NULL_PTR(memo->others) ? DT_FILE_REFERENCE_OTHERS
                                                                         : DT_FILE_REFERENCE_MISSING;
  g_mutex_unlock(&_memo_lock);
  return status;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
