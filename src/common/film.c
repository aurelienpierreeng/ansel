/*
   This file is part of darktable,
   Copyright (C) 2009-2013 johannes hanika.
   Copyright (C) 2010-2012 Henrik Andersson.
   Copyright (C) 2010-2018, 2020 Tobias Ellinghaus.
   Copyright (C) 2012 James C. McPherson.
   Copyright (C) 2012 Jesper Pedersen.
   Copyright (C) 2012 José Carlos García Sogo.
   Copyright (C) 2012 Richard Wonka.
   Copyright (C) 2013 Dennis Gnad.
   Copyright (C) 2013-2015 Jérémy Rosen.
   Copyright (C) 2013-2014, 2020-2021 Pascal Obry.
   Copyright (C) 2013 Simon Spannagel.
   Copyright (C) 2014-2016 Roman Lebedev.
   Copyright (C) 2017 luzpaz.
   Copyright (C) 2018 Edgardo Hoszowski.
   Copyright (C) 2018 parafin.
   Copyright (C) 2019, 2022, 2025 Aurélien PIERRE.
   Copyright (C) 2019-2020 Hanno Schwalm.
   Copyright (C) 2019, 2021-2022 Philippe Weyland.
   Copyright (C) 2020-2021 Aldric Renaudin.
   Copyright (C) 2020 Heiko Bauke.
   Copyright (C) 2020 Hubert Kowalski.
   Copyright (C) 2020 JP Verrue.
   Copyright (C) 2020 Nicolas Auffray.
   Copyright (C) 2021-2022 HansBull.
   Copyright (C) 2021 Ralf Brown.
   Copyright (C) 2022 Martin Bařinka.
   
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
#include "common/film.h"
#include "control/settings.h"
#include "common/collection.h"
#include "common/mipmap_cache.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/image_cache.h"
#include "common/tags.h"
#include "common/conf.h"
#include "control/control.h"
#include "control/jobs/film_jobs.h"
#include "control/jobs.h"
#include "views/view.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "common/utility.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

void dt_film_init(dt_film_t *film)
{
  dt_pthread_mutex_init(&film->images_mutex, NULL);
  film->last_loaded = film->num_images = 0;
  film->dirname[0] = '\0';
  film->dir = NULL;
  film->id = -1;
  film->ref = 0;
}

void dt_film_cleanup(dt_film_t *film)
{
  dt_pthread_mutex_destroy(&film->images_mutex);
  if(film->dir)
  {
    g_dir_close(film->dir);
    film->dir = NULL;
  }
}

void dt_film_set_query(const int32_t id)
{
  /* enable film id filter and set film id */
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", 0);
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, folder"
                              " FROM main.film_rolls"
                              " WHERE id = ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_conf_set_string("plugins/lighttable/collect/string0", (gchar *)sqlite3_column_text(stmt, 1));
  }
  sqlite3_finalize(stmt);
  dt_collection_update_query(dt_collection_get_global(), DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

int32_t dt_film_get_id(const char *folder)
{
  int32_t filmroll_id = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
#ifdef _WIN32
                              "SELECT id FROM main.film_rolls WHERE folder LIKE ?1",
#else
                              "SELECT id FROM main.film_rolls WHERE folder = ?1",
#endif
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, folder, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) filmroll_id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return filmroll_id;
}

int dt_film_open(const int32_t id)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, folder"
                              " FROM main.film_rolls"
                              " WHERE id = ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_finalize(stmt);

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE main.film_rolls"
                                " SET access_timestamp = strftime('%s', 'now')"
                                " WHERE id = ?1", -1, &stmt,
                                NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  // TODO: prefetch to cache using image_open
  dt_film_set_query(id);
  dt_control_queue_redraw_center();
  dt_view_manager_reset(dt_view_manager_get_global());
  return 0;
}


int dt_film_new(dt_film_t *film, const char *directory)
{
  sqlite3_stmt *stmt;

  // Try open filmroll for folder if exists
  film->id = -1;
  g_strlcpy(film->dirname, directory, sizeof(film->dirname));

  // remove a closing '/', unless it's also the start
  char *last = &film->dirname[strlen(film->dirname) - 1];
  if(*last == '/' && last != film->dirname) *last = '\0';

  /* if we didn't find an id, lets instantiate a new filmroll */
  film->id = dt_film_get_id(film->dirname);

  /* if we didn't find an id, lets instantiate a new filmroll */
  if(film->id <= 0)
  {
    // create a new filmroll
    /* insert a new film roll into database */
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "INSERT INTO main.film_rolls (id, access_timestamp, folder)"
                                "  VALUES (NULL, strftime('%s', 'now'), ?1)",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, film->dirname, -1, SQLITE_STATIC);
    const int rc = sqlite3_step(stmt);
    if(rc != SQLITE_DONE)
      fprintf(stderr, "[film_new] failed to insert film roll! %s\n",
              sqlite3_errmsg(dt_database_get_sqlite3_global()));
    sqlite3_finalize(stmt);
    /* requery for filmroll and fetch new id */
    film->id = dt_film_get_id(film->dirname);
    if(film->id)
    {
      // add it to the table memory.film_folder
      sqlite3_stmt *stmt2;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                  "INSERT INTO memory.film_folder (id, status) "
                                  "VALUES (?1, 1)",
                                  -1, &stmt2, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, film->id);
      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);
    }
  }
#ifdef _WIN32
  else
  {
    // make sure we reuse the same path case
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT folder FROM main.film_rolls WHERE id = ?1",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film->id);
    if(sqlite3_step(stmt) != SQLITE_ROW)
      g_strlcpy(film->dirname, (const char *)sqlite3_column_text(stmt, 0), sizeof(film->dirname));
    sqlite3_finalize(stmt);
  }
#endif

  if(film->id <= 0)
    dt_print(DT_DEBUG_IMPORT, "[Import] Could not create a new filmid for %s\n", directory);
  else
    dt_print(DT_DEBUG_IMPORT, "[Import] Reusing or creating filmid %i for %s\n", film->id, directory);

  if(film->id <= 0) return 0;
  film->last_loaded = 0;
  return film->id;
}

int dt_film_import(const char *dirname)
{
  GError *error = NULL;

  /* initialize a film object*/
  dt_film_t *film = (dt_film_t *)malloc(sizeof(dt_film_t));
  dt_film_init(film);

  dt_film_new(film, dirname);

  /* bail out if we got troubles */
  if(film->id <= 0)
  {
    // if the film is empty => remove it again.
    if(dt_film_is_empty(film->id))
    {
      dt_film_remove(film->id);
    }
    dt_film_cleanup(film);
    dt_free(film);
    return 0;
  }

  // when called without job system running the import will be done synchronously and destroy the film object
  const int filmid = film->id;

  /* at last put import film job on queue */
  film->last_loaded = 0;
  film->dir = g_dir_open(film->dirname, 0, &error);
  if(error)
  {
    fprintf(stderr, "[film_import] failed to open directory %s: %s\n", film->dirname, error->message);
    g_error_free(error);
    dt_film_cleanup(film);
    dt_free(film);
    return 0;
  }

  // launch import job
  dt_control_add_job(dt_control_get_global(), DT_JOB_QUEUE_USER_BG, dt_film_import1_create(film));

  return filmid;
}

static dt_film_confirm_rmdir_handler_t _confirm_rmdir_handler = NULL;

void dt_film_set_confirm_rmdir_handler(dt_film_confirm_rmdir_handler_t handler)
{
  _confirm_rmdir_handler = handler;
}

void dt_film_remove_directories(const GList *dirs)
{
  for(const GList *iter = dirs; iter; iter = g_list_next(iter))
    rmdir((char *)iter->data);
}

void dt_film_remove_empty()
{
  // remove all empty film rolls from db:
  GList *empty_dirs = NULL;
  gboolean ask_before_rmdir = dt_conf_get_bool("ask_before_rmdir");
  gboolean raise_signal = FALSE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id,folder"
                              " FROM main.film_rolls AS B"
                              " WHERE (SELECT COUNT(*)"
                              "        FROM main.images AS A"
                              "        WHERE A.film_id=B.id) = 0",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_stmt *inner_stmt;
    raise_signal = TRUE;
    const gint id = sqlite3_column_int(stmt, 0);
    const gchar *folder = (const gchar *)sqlite3_column_text(stmt, 1);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "DELETE FROM main.film_rolls WHERE id=?1", -1,
                                &inner_stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(inner_stmt, 1, id);
    sqlite3_step(inner_stmt);
    sqlite3_finalize(inner_stmt);

    if(dt_util_is_dir_empty(folder))
    {
      if(ask_before_rmdir) empty_dirs = g_list_prepend(empty_dirs, g_strdup(folder));
      else rmdir(folder);
    }
  }
  sqlite3_finalize(stmt);
  if(raise_signal) DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_FILMROLLS_REMOVED);

  // dispatch asking for deletion (and subsequent deletion) to the gui thread
  if(empty_dirs)
  {
    empty_dirs = g_list_reverse(empty_dirs);
    // Nobody to ask -> nothing is deleted. "ask before rmdir" cannot be honoured headless.
    if(_confirm_rmdir_handler)
      _confirm_rmdir_handler(empty_dirs);   // takes ownership
    else
      g_list_free_full(empty_dirs, dt_free_gpointer);
    empty_dirs = NULL;
  }
}

gboolean dt_film_is_empty(const int id)
{
  gboolean empty = FALSE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.images WHERE film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) != SQLITE_ROW) empty = TRUE;
  sqlite3_finalize(stmt);
  return empty;
}

// This is basically the same as dt_image_remove() from common/image.c.
// It just does the iteration over all images in the SQL statement
void dt_film_remove(const int id)
{
  // only allowed if local copies have their original accessible

  sqlite3_stmt *stmt;

  gboolean remove_ok = TRUE;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.images WHERE film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t imgid = sqlite3_column_int(stmt, 0);
    if(!dt_image_safe_remove(imgid))
    {
      remove_ok = FALSE;
      break;
    }
  }
  sqlite3_finalize(stmt);

  if(!remove_ok)
  {
    dt_control_log(_("cannot remove film roll having local copies with non accessible originals"));
    return;
  }

  // query is needed a second time for mipmap and image cache
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.images WHERE film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t imgid = sqlite3_column_int(stmt, 0);
    dt_image_local_copy_reset(imgid);
    dt_mipmap_cache_remove(dt_mipmap_cache_get_global(), imgid, TRUE);
    dt_image_cache_remove(dt_image_cache_get_global(), imgid);
  }
  sqlite3_finalize(stmt);

  // due to foreign keys, all images with references to the film roll are deleted,
  // and likewise all entries with references to those images
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM main.film_rolls WHERE id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // dt_control_update_recent_films();

  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_FILMROLLS_CHANGED);
}

void dt_film_relocate(const char *old_path, const char *new_path)
{
  if(IS_NULL_PTR(old_path) || IS_NULL_PTR(new_path)) return;

  // Gather every film roll under old_path together with its remapped folder first, so we do
  // not mutate the table while still iterating the SELECT.
  sqlite3_stmt *stmt;
  gchar *like = g_strdup_printf("%s%%", old_path);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, folder FROM main.film_rolls WHERE folder LIKE ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, like, -1, SQLITE_TRANSIENT);
  g_free(like);

  GList *ids = NULL;
  GList *folders = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    const gchar *old = (const gchar *)sqlite3_column_text(stmt, 1);
    gchar *final = g_strcmp0(old, old_path) ? g_strdup_printf("%s/%s", new_path, old + strlen(old_path) + 1)
                                            : g_strdup(new_path);
    ids = g_list_prepend(ids, GINT_TO_POINTER(id));
    folders = g_list_prepend(folders, final);
  }
  sqlite3_finalize(stmt);

  sqlite3_stmt *up;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE main.film_rolls SET folder=?1 WHERE id=?2", -1, &up, NULL);
  for(GList *i = ids, *f = folders; i && f; i = g_list_next(i), f = g_list_next(f))
  {
    sqlite3_reset(up);
    sqlite3_clear_bindings(up);
    DT_DEBUG_SQLITE3_BIND_TEXT(up, 1, (const char *)f->data, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(up, 2, GPOINTER_TO_INT(i->data));
    sqlite3_step(up);
  }
  sqlite3_finalize(up);
  g_list_free(ids);
  g_list_free_full(folders, g_free);
}

GList *dt_film_get_image_ids(const int filmid)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.images WHERE film_id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    result = g_list_prepend(result, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

void dt_film_set_folder_status()
{
  sqlite3_stmt *stmt, *stmt2;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM memory.film_folder",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, folder FROM main.film_rolls",
                              -1, &stmt, NULL);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO memory.film_folder (id, status) "
                              "VALUES (?1, ?2)",
                              -1, &stmt2, NULL);
  // clang-format on

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int filmid = sqlite3_column_int(stmt, 0);
    const char *folder = (char *)sqlite3_column_text(stmt, 1);
    const int status = g_file_test(folder, G_FILE_TEST_IS_DIR);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, filmid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, status);
    sqlite3_step(stmt2);
    sqlite3_reset(stmt2);
  }
  sqlite3_finalize(stmt);
  sqlite3_finalize(stmt2);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
