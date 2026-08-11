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
#include "database/database.h"
#include "database/film_repository.h"
#include "control/settings.h"
#include "common/collection.h"
#include "caches/mipmap_cache.h"
#include "system/dtpthread.h"
#include "caches/image_cache.h"
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
  char *folder = dt_film_repository_get_folder(id);
  if(folder)
  {
    dt_conf_set_string("plugins/lighttable/collect/string0", folder);
    dt_free(folder);
  }
  dt_collection_update_query(dt_collection_get_global(), DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

int32_t dt_film_get_id(const char *folder)
{
  return dt_film_repository_find_by_folder(folder);
}

int dt_film_open(const int32_t id)
{
  char *folder = dt_film_repository_get_folder(id);
  if(folder)
  {
    dt_film_repository_touch_access(id);
    dt_free(folder);
  }
  // TODO: prefetch to cache using image_open
  dt_film_set_query(id);
  dt_control_queue_redraw_center();
  dt_view_manager_reset(dt_view_manager_get_global());
  return 0;
}


int dt_film_new(dt_film_t *film, const char *directory)
{
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
    if(!dt_film_repository_insert(film->dirname))
      fprintf(stderr, "[film_new] failed to insert film roll! %s\n", dt_database_get_last_error());
    /* requery for filmroll and fetch new id */
    film->id = dt_film_get_id(film->dirname);
    if(film->id)
    {
      // add it to the table memory.film_folder
      dt_film_repository_folder_status_set(film->id, TRUE);
    }
  }
#ifdef _WIN32
  else
  {
    // Make sure we reuse the same path case.
    //
    // dt_film_get_id() matches case-insensitively on Windows (LIKE), so film->dirname can differ
    // in case from what the roll is actually stored under; adopt the stored spelling so the two
    // agree. This used to test `sqlite3_step(...) != SQLITE_ROW` and then read column 0 of a
    // statement that had returned no row -- i.e. it could only ever have copied a NULL, and
    // since film->id comes from dt_film_get_id() just above the row always exists, so the copy
    // never ran at all. The test is inverted here to what it was plainly meant to be.
    char *stored = dt_film_repository_get_folder(film->id);
    if(stored)
    {
      g_strlcpy(film->dirname, stored, sizeof(film->dirname));
      dt_free(stored);
    }
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


typedef struct _empty_rolls_t
{
  GList *ids;
  GList *folders;
} _empty_rolls_t;

static void _collect_empty_roll(void *user_data, const int32_t id, const char *folder)
{
  _empty_rolls_t *r = (_empty_rolls_t *)user_data;
  r->ids = g_list_prepend(r->ids, GINT_TO_POINTER(id));
  r->folders = g_list_prepend(r->folders, g_strdup(folder));
}

typedef struct _relocation_t
{
  const char *old_path;
  const char *new_path;
  GList *ids;
  GList *folders;
} _relocation_t;

static void _collect_relocation(void *user_data, const int32_t id, const char *folder)
{
  _relocation_t *r = (_relocation_t *)user_data;
  gchar *final = g_strcmp0(folder, r->old_path)
                     ? g_strdup_printf("%s/%s", r->new_path, folder + strlen(r->old_path) + 1)
                     : g_strdup(r->new_path);
  r->ids = g_list_prepend(r->ids, GINT_TO_POINTER(id));
  r->folders = g_list_prepend(r->folders, final);
}

void dt_film_remove_empty()
{
  // remove all empty film rolls from db:
  GList *empty_dirs = NULL;
  gboolean ask_before_rmdir = dt_conf_get_bool("ask_before_rmdir");
  gboolean raise_signal = FALSE;
  // Collect first: the original DELETEd rows while its own SELECT cursor was still walking
  // main.film_rolls.
  _empty_rolls_t rolls = { NULL, NULL };
  dt_film_repository_foreach_empty(_collect_empty_roll, &rolls);
  rolls.ids = g_list_reverse(rolls.ids);
  rolls.folders = g_list_reverse(rolls.folders);

  for(GList *i = rolls.ids, *f = rolls.folders; i && f; i = g_list_next(i), f = g_list_next(f))
  {
    raise_signal = TRUE;
    const gchar *folder = (const gchar *)f->data;
    dt_film_repository_delete(GPOINTER_TO_INT(i->data));

    if(dt_util_is_dir_empty(folder))
    {
      if(ask_before_rmdir) empty_dirs = g_list_prepend(empty_dirs, g_strdup(folder));
      else rmdir(folder);
    }
  }
  g_list_free(rolls.ids);
  g_list_free_full(rolls.folders, g_free);
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
  return !dt_film_repository_has_images(id);
}

// This is basically the same as dt_image_remove() from common/image.c.
// It just does the iteration over all images in the SQL statement
void dt_film_remove(const int id)
{
  // only allowed if local copies have their original accessible

  gboolean remove_ok = TRUE;

  GList *imgids = dt_film_repository_get_image_ids(id);

  for(GList *l = imgids; l; l = g_list_next(l))
  {
    if(!dt_image_safe_remove(GPOINTER_TO_INT(l->data)))
    {
      remove_ok = FALSE;
      break;
    }
  }

  if(!remove_ok)
  {
    g_list_free(imgids);
    dt_control_log(_("cannot remove film roll having local copies with non accessible originals"));
    return;
  }

  // the same ids again, this time for the mipmap and image caches
  for(GList *l = imgids; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    dt_image_local_copy_reset(imgid);
    dt_mipmap_cache_remove(imgid, TRUE);
    dt_image_cache_remove(imgid);
  }
  g_list_free(imgids);

  dt_film_repository_delete(id);
  // dt_control_update_recent_films();

  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_FILMROLLS_CHANGED);
}

void dt_film_relocate(const char *old_path, const char *new_path)
{
  if(IS_NULL_PTR(old_path) || IS_NULL_PTR(new_path)) return;

  // Gather every film roll under old_path together with its remapped folder first, so we do
  // not mutate the table while still iterating the SELECT.
  _relocation_t reloc = { old_path, new_path, NULL, NULL };
  dt_film_repository_foreach_under(old_path, _collect_relocation, &reloc);
  reloc.ids = g_list_reverse(reloc.ids);
  reloc.folders = g_list_reverse(reloc.folders);

  for(GList *i = reloc.ids, *f = reloc.folders; i && f; i = g_list_next(i), f = g_list_next(f))
    dt_film_repository_set_folder(GPOINTER_TO_INT(i->data), (const char *)f->data);

  g_list_free(reloc.ids);
  g_list_free_full(reloc.folders, g_free);
}

GList *dt_film_get_image_ids(const int filmid)
{
  return dt_film_repository_get_image_ids(filmid);
}

static void _record_folder_status(void *user_data, const int32_t id, const char *folder)
{
  (void)user_data;
  dt_film_repository_folder_status_set(id, g_file_test(folder, G_FILE_TEST_IS_DIR));
}

void dt_film_set_folder_status()
{
  // Writing memory.film_folder while the main.film_rolls cursor is open is safe -- different
  // tables -- and this is what the original did, one INSERT per row as it read them.
  dt_film_repository_folder_status_clear();
  dt_film_repository_foreach(_record_folder_status, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
