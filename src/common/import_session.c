/*
    This file is part of darktable,
    Copyright (C) 2014-2021 darktable developers.

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

#include <stdio.h>

#include "common/film.h"
#include "common/import_session.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"

/* TODO: Investigate if we can make one import session instance thread safe
         eg. having several background jobs working with same instance.
*/

typedef struct dt_import_session_t
{
  uint32_t ref;

  dt_film_t *film;
  dt_variables_params_t *vp;

  gchar *current_path;
  gchar *current_filename;

} dt_import_session_t;


static void _import_session_cleanup_filmroll(dt_import_session_t *self)
{
  if(self->film == NULL) return;
  /* if current filmroll for session is empty, remove it */
  if(dt_film_is_empty(self->film->id))
  {
    dt_film_remove(self->film->id);
    if(self->current_path != NULL && g_file_test(self->current_path, G_FILE_TEST_IS_DIR) && dt_util_is_dir_empty(self->current_path))
    {
      // no need to ask for rmdir as it'll be re-created if it's needed
      // by another import session with same path params
      g_rmdir(self->current_path);
      g_free(self->current_path);
      self->current_path = NULL;
    }
  }
  dt_film_cleanup(self->film);

  g_free(self->film);
  self->film = NULL;
}


static gboolean _import_session_initialize_filmroll(dt_import_session_t *self, char *path)
{
  /* cleanup of previously used filmroll */
  _import_session_cleanup_filmroll(self);

  /* recursively create directories, abort if failed */
  if(g_mkdir_with_parents(path, 0755) == -1)
  {
    fprintf(stderr, "failed to create session path %s.\n", path);
    _import_session_cleanup_filmroll(self);
    return TRUE;
  }
  /* open one or initialize a filmroll for the session */
  self->film = (dt_film_t *)g_malloc0(sizeof(dt_film_t));
  const int32_t film_id = dt_film_new(self->film, path);
  if(film_id == 0)
  {
    fprintf(stderr, "[import_session] Failed to initialize film roll.\n");
    _import_session_cleanup_filmroll(self);
    return TRUE;
  }
  /* every thing is good lets setup current path */
#ifdef _WIN32
  else
  {
    // keep the existing film path (case)
    g_free(path);
    g_free(self->current_path);
    dt_film_t *film = self->film;
    self->current_path = g_strdup(film->dirname);
  }
#else
  g_free(self->current_path);
  self->current_path = path;
#endif

  return FALSE;
}


static void _import_session_migrate_old_config()
{
  /* TODO: check if old config exists, migrate to new and remove old */
}


static gchar *_import_session_path_pattern()
{
  gchar *result = NULL;
  const char *base = dt_conf_get_string_const("session/base_directory_pattern");
  const char *sub  = dt_conf_get_string_const("session/sub_directory_pattern");

  if(!sub || !base)
  {
    fprintf(stderr, "[import_session] No base or subpath configured...\n");
    return NULL;
  }
  else
    result = g_build_path(G_DIR_SEPARATOR_S, base, sub, (char *)NULL);

  return result;
}


static char *_import_session_filename_pattern()
{
  gchar *name = dt_conf_get_string("session/filename_pattern");
  if(!name)
  {
    fprintf(stderr, "[import_session] No name configured...\n");
    return NULL;
  }

  return name;
}


struct dt_import_session_t *dt_import_session_new()
{
  dt_import_session_t *is;

  is = (dt_import_session_t *)g_malloc0(sizeof(dt_import_session_t));

  dt_variables_params_init(&is->vp);

  /* migrate old configuration */
  _import_session_migrate_old_config();
  return is;
}


void dt_import_session_destroy(struct dt_import_session_t *self)
{
  if(--self->ref != 0) return;

  /* cleanup of session import film roll */
  _import_session_cleanup_filmroll(self);

  dt_variables_params_destroy(self->vp);

  g_free(self);
}

gboolean dt_import_session_ready(struct dt_import_session_t *self)
{
  return (self->film && self->film->id);
}

void dt_import_session_ref(struct dt_import_session_t *self)
{
  self->ref++;
}

void dt_import_session_unref(struct dt_import_session_t *self)
{
  self->ref--;
}

void dt_import_session_import(struct dt_import_session_t *self)
{
  const int32_t id = dt_image_import(self->film->id, self->current_filename, TRUE);
  if(id)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, id);
    dt_control_queue_redraw();
  }
}


void dt_import_session_set_name(struct dt_import_session_t *self, const char *name)
{
  /* free previous jobcode name */
  g_free((void *)self->vp->jobcode);

  self->vp->jobcode = g_strdup(name);
}


void dt_import_session_set_time(struct dt_import_session_t *self, const char *time)
{
  dt_variables_set_time(self->vp, time);
}


void
dt_import_session_set_exif_time(struct dt_import_session_t *self, const char *exif_time)
{
  dt_variables_set_exif_time(self->vp, exif_time);
}


void dt_import_session_set_filename(struct dt_import_session_t *self, const char *filename)
{
  self->vp->filename = filename;
}


int32_t dt_import_session_film_id(struct dt_import_session_t *self)
{
  if(self->film) return self->film->id;

  return -1;
}


const char *dt_import_session_name(struct dt_import_session_t *self)
{
  return self->vp->jobcode;
}

/* This returns a unique filename using session path **and** the filename.
   If current is true we will use the original filename otherwise use the pattern.
*/
const char *dt_import_session_filename(struct dt_import_session_t *self)
{
  gchar *result_fname = NULL;

  /* expand next filename */
  g_free(self->current_filename);
  self->current_filename = NULL;

  char *pattern = _import_session_filename_pattern();
  if(pattern == NULL)
  {
    fprintf(stderr, "[import_session] Failed to get session filaname pattern.\n");
    return NULL;
  }

  /* verify that expanded path and filename yields a unique file */
  // NOTE: self->current_path is edited in dt_import_session_path()
  const char *path = (self->current_path) ? self->current_path : dt_import_session_path(self, TRUE);

  result_fname = dt_variables_expand(self->vp, pattern, TRUE);

  char *fname = g_build_path(G_DIR_SEPARATOR_S, path, result_fname, (char *)NULL);
  char *previous_fname = fname;
  if(g_file_test(fname, G_FILE_TEST_EXISTS) == TRUE)
  {
    fprintf(stderr, "[import_session] File %s exists.\n", fname);
    do
    {
      /* file exists, yield a new filename */
      g_free(result_fname);
      result_fname = dt_variables_expand(self->vp, pattern, TRUE);
      fname = g_build_path(G_DIR_SEPARATOR_S, path, result_fname, (char *)NULL);

      fprintf(stderr, "[import_session] Testing %s.\n", fname);
      /* check if same filename was yielded as before */
      if(strcmp(previous_fname, fname) == 0)
      {
        g_free(previous_fname);
        g_free(fname);
        dt_control_log(_(
            "couldn't expand to a unique filename for session, please check your import session settings."));
        return NULL;
      }

      g_free(previous_fname);
      previous_fname = fname;

    } while(g_file_test(fname, G_FILE_TEST_EXISTS) == TRUE);
  }

  g_free(previous_fname);
  g_free(pattern);

  self->current_filename = result_fname;
  fprintf(stderr, "[import_session] Using filename %s.\n", self->current_filename);

  return self->current_filename;
}

static const char *_import_session_path(struct dt_import_session_t *self, gboolean current)
{
  const gboolean currentok = dt_util_test_writable_dir(self->current_path);

  if(current && self->current_path != NULL)
  {
    // the current path might not be a writable directory so test for that
    if(currentok) return self->current_path;
    // the current path is not valid so we can't  cleanup
    g_free(self->current_path);
    self->current_path = NULL;
    return NULL;
  }

  gchar *pattern = _import_session_path_pattern();
  if(pattern == NULL)
  {
    fprintf(stderr, "[import_session] Failed to get session path pattern.\n");
    return NULL;
  }

  gchar *new_path = dt_variables_expand(self->vp, pattern, FALSE);
  g_free(pattern);

#ifdef WIN32
  if(new_path && (strlen(new_path) > 1))
  {
    const char first = g_ascii_toupper(new_path[0]);
    if(first >= 'A' && first <= 'Z' && new_path[1] == ':') // path format is <drive letter>:\path\to\file
      new_path[0] = first;                                 // drive letter in uppercase looks nicer
  }
#endif


  /* did the session path change ?*/
  if(self->current_path && strcmp(self->current_path, new_path) == 0)
  {
    g_free(new_path);
    new_path = NULL;
    if(currentok) return self->current_path;
  }

  if(!currentok)
  {
    g_free(self->current_path);
    self->current_path = NULL;
  }
  /* we need to initialize a new filmroll for the new path */
  if(_import_session_initialize_filmroll(self, new_path) != 0)
  {
    g_free(new_path);
    return NULL;
  }
  return self->current_path;
}

const char *dt_import_session_path(struct dt_import_session_t *self, gboolean current)
{
  const char *path = _import_session_path(self, current);
  if(path == NULL)
  {
    fprintf(stderr, "[import_session] Failed to get session path.\n");
    dt_control_log(_("requested session path not available. "
                     "device not mounted?"));
  }
  return path;
}

// Checks for the opposite separator in a string and replace it by the needed one by the current OS
void dt_str_replace_bad_separator(gchar **string)
{
#ifdef WIN32
  *string = dt_str_replace(*string, "/", G_DIR_SEPARATOR_S);
#else
  *string = dt_str_replace(*string, "\\", G_DIR_SEPARATOR_S);
#endif
}


static const char *_import_session_total(struct dt_import_session_t *self)
{
  gchar *path_pattern = _import_session_path_pattern();

  if(path_pattern == NULL)
  {
    fprintf(stderr, "[import_session] Failed to get session path pattern.\n");
    return NULL;
  }

  gchar *filename_pattern = _import_session_filename_pattern();
  if(filename_pattern == NULL)
  {
    fprintf(stderr, "[import_session] Failed to get session file name pattern.\n");
    return NULL;
  }

  gchar *pattern = g_build_filename(path_pattern, filename_pattern, NULL);
  g_free(path_pattern);
  g_free(filename_pattern);

  gchar *result_path = dt_variables_expand(self->vp, pattern, FALSE);
  g_free(pattern);

  if(result_path == NULL)
    return NULL;

  // this would need to be removed if we decide to do the correction on user's settings directly
  dt_str_replace_bad_separator(&result_path);

#ifdef WIN32
  if(result_path && (strlen(result_path) > 1))
  {
    const char first = g_ascii_toupper(result_path[0]);
    if(first >= 'A' && first <= 'Z' && result_path[1] == ':') // path format is <drive letter>:\path\to\file
      result_path[0] = first;                                 // drive letter in uppercase looks nicer
  }
#endif

  self->current_path = g_strdup(result_path);

  g_free(result_path);
  return self->current_path;
}

const char *dt_import_session_total(struct dt_import_session_t *self)
{
  const char *path = _import_session_total(self);
  if(path == NULL)
  {
    fprintf(stderr, "[import_session] Failed to get session path.\n");
    dt_control_log(_("requested session path not available. "
                     "device not mounted?"));
  }
  return path;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
