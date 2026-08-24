/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Guillaume STUTIN.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
*/

#include "system/mem_alloc.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "common/folder_survey.h"
#include "control/settings.h"
#include <glib/gstdio.h>
#include "common/image_extensions.h"

#include "common/datetime.h"
#include "common/file_location.h"
#include "common/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/jobs/import_jobs.h"

#include <gio/gio.h>
#include "common/utility.h"
#include "common/logging.h"

#define DT_FOLDER_SURVEY_STATE_FILE "folder-survey-state.ini"

typedef enum dt_folder_survey_file_state_t
{
  DT_FOLDER_SURVEY_FILE_PENDING = 0,
  DT_FOLDER_SURVEY_FILE_QUEUED,
  DT_FOLDER_SURVEY_FILE_DONE
} dt_folder_survey_file_state_t;

typedef struct dt_folder_survey_entry_t
{
  guint64 size;
  gint64 mtime;
  int stable_scans;
  dt_folder_survey_file_state_t state;
} dt_folder_survey_entry_t;

typedef struct dt_folder_survey_observation_t
{
  guint64 size;
  gint64 mtime;
} dt_folder_survey_observation_t;

typedef struct dt_folder_survey_job_t
{
  char *folder;
  guint generation;
} dt_folder_survey_job_t;

typedef struct dt_folder_survey_t
{
  dt_pthread_mutex_t lock;
  GHashTable *files;
  char *folder;
  char *state_path;
  guint generation;
  guint timer;
  guint immediate_scan;
  gboolean initialized;
  gboolean active;
  gboolean baseline_initialized;
  gboolean scan_running;
  gboolean shutting_down;
  // TRUE when the previous application session quit while monitoring.
  gboolean was_active_last_session;
} dt_folder_survey_t;

static dt_folder_survey_t _folder_survey = { 0 };

/**
 * @brief Persist the complete survey baseline while the survey lock is held.
 *
 * The state file is replaced as one transaction so a crash cannot leave a
 * partially-written list that would cause old files to be treated as new.
 */
static int _folder_survey_save_locked()
{
  GKeyFile *state = g_key_file_new();
  g_key_file_set_string(state, "survey", "folder", _folder_survey.folder ? _folder_survey.folder : "");
  g_key_file_set_boolean(state, "survey", "initialized", _folder_survey.baseline_initialized);
  // Session marker: an application shutdown while monitoring leaves this TRUE
  // so the next start can propose to resume the studio session.
  g_key_file_set_boolean(state, "survey", "active", _folder_survey.active);

  GHashTableIter iter;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, _folder_survey.files);
  // Serialize every known image path and the metadata used to detect changes.
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    const char *path = (const char *)key;
    const dt_folder_survey_entry_t *entry = (const dt_folder_survey_entry_t *)value;
    char *group = g_compute_checksum_for_string(G_CHECKSUM_SHA256, path, -1);
    g_key_file_set_string(state, group, "path", path);
    g_key_file_set_uint64(state, group, "size", entry->size);
    g_key_file_set_int64(state, group, "mtime", entry->mtime);
    g_key_file_set_integer(state, group, "stable_scans", entry->stable_scans);
    g_key_file_set_integer(state, group, "state", entry->state);
    dt_free(group);
  }

  gsize length = 0;
  gchar *contents = g_key_file_to_data(state, &length, NULL);
  GFile *file = g_file_new_for_path(_folder_survey.state_path);
  GError *error = NULL;
  const gboolean success = g_file_replace_contents(file, contents, length, NULL, FALSE,
                                                   G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, &error);
  if(!success)
  {
    fprintf(stderr, "[folder survey] failed to save state: %s\n", error ? error->message : "unknown error");
    g_clear_error(&error);
  }

  g_object_unref(file);
  dt_free(contents);
  g_key_file_unref(state);
  return success ? 0 : 1;
}

/**
 * @brief Load the previous file list and convert interrupted imports to retries.
 */
static int _folder_survey_load_locked()
{
  GKeyFile *state = g_key_file_new();
  GError *error = NULL;
  if(!g_key_file_load_from_file(state, _folder_survey.state_path, G_KEY_FILE_NONE, &error))
  {
    g_clear_error(&error);
    g_key_file_unref(state);
    return 0;
  }

  dt_free(_folder_survey.folder);
  _folder_survey.folder = g_key_file_get_string(state, "survey", "folder", NULL);
  _folder_survey.baseline_initialized = g_key_file_get_boolean(state, "survey", "initialized", NULL);
  _folder_survey.was_active_last_session = g_key_file_get_boolean(state, "survey", "active", NULL);

  gsize groups_count = 0;
  gchar **groups = g_key_file_get_groups(state, &groups_count);
  // Restore one image observation from each hashed path group.
  for(gsize k = 0; k < groups_count; k++)
  {
    if(!strcmp(groups[k], "survey")) continue;

    char *path = g_key_file_get_string(state, groups[k], "path", NULL);
    if(IS_NULL_PTR(path) || path[0] == '\0')
    {
      dt_free(path);
      continue;
    }

    dt_folder_survey_entry_t *entry = calloc(1, sizeof(dt_folder_survey_entry_t));
    if(IS_NULL_PTR(entry))
    {
      dt_free(path);
      continue;
    }

    entry->size = g_key_file_get_uint64(state, groups[k], "size", NULL);
    entry->mtime = g_key_file_get_int64(state, groups[k], "mtime", NULL);
    entry->stable_scans = g_key_file_get_integer(state, groups[k], "stable_scans", NULL);
    entry->state = g_key_file_get_integer(state, groups[k], "state", NULL);
    if(entry->state < DT_FOLDER_SURVEY_FILE_PENDING || entry->state > DT_FOLDER_SURVEY_FILE_DONE
       || entry->state == DT_FOLDER_SURVEY_FILE_QUEUED)
    {
      entry->state = DT_FOLDER_SURVEY_FILE_PENDING;
      entry->stable_scans = 0;
    }
    g_hash_table_replace(_folder_survey.files, path, entry);
  }

  g_strfreev(groups);
  g_key_file_unref(state);
  return 0;
}

/**
 * @brief Recursively collect supported images and their stability metadata.
 *
 * The loop looks only for regular image files. Directories are traversed in
 * place so a selected ingest root behaves like selecting a folder in Import.
 */
static int _folder_survey_collect(const char *folder, GHashTable *observed)
{
  GQueue folders = G_QUEUE_INIT;
  g_queue_push_tail(&folders, g_file_new_for_path(folder));
  int error = 0;

  // Traverse every directory below the configured ingest root.
  while(!g_queue_is_empty(&folders))
  {
    GFile *current = g_queue_pop_head(&folders);
    GError *enumeration_error = NULL;
    GFileEnumerator *enumerator = g_file_enumerate_children(
        current,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE
                                       "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NONE, NULL, &enumeration_error);
    g_object_unref(current);

    if(IS_NULL_PTR(enumerator))
    {
      fprintf(stderr, "[folder survey] failed to enumerate folder: %s\n",
              enumeration_error ? enumeration_error->message : "unknown error");
      g_clear_error(&enumeration_error);
      error = 1;
      break;
    }

    GFileInfo *info = NULL;
    GFile *child = NULL;
    // Record supported regular images and queue child directories for traversal.
    while(g_file_enumerator_iterate(enumerator, &info, &child, NULL, &enumeration_error))
    {
      if(IS_NULL_PTR(info) || IS_NULL_PTR(child)) break;

      const GFileType type = g_file_info_get_file_type(info);
      if(type == G_FILE_TYPE_DIRECTORY)
      {
        g_queue_push_tail(&folders, g_object_ref(child));
        continue;
      }
      if(type != G_FILE_TYPE_REGULAR) continue;

      char *path = g_file_get_path(child);
      if(IS_NULL_PTR(path) || !dt_supported_image(path))
      {
        dt_free(path);
        continue;
      }

      char *canonical_path = g_canonicalize_filename(path, NULL);
      dt_folder_survey_observation_t *observation = malloc(sizeof(dt_folder_survey_observation_t));
      if(!IS_NULL_PTR(observation))
      {
        observation->size = g_file_info_get_size(info);
        observation->mtime = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        g_hash_table_replace(observed, canonical_path, observation);
        canonical_path = NULL;
      }
      dt_free(canonical_path);
      dt_free(path);
    }

    if(!IS_NULL_PTR(enumeration_error))
    {
      fprintf(stderr, "[folder survey] failed while enumerating files: %s\n", enumeration_error->message);
      g_clear_error(&enumeration_error);
      error = 1;
    }
    g_object_unref(enumerator);
    if(error) break;
  }

  while(!g_queue_is_empty(&folders)) g_object_unref(g_queue_pop_head(&folders));
  return error;
}

/**
 * @brief Read the ordered auto-apply style list from conf.
 *
 * Styles are a Studio Capture feature, the survey runs in the background
 * regardless of the active view, so styles are applied even if the user
 * has switched away from Studio Capture while a session keeps monitoring.
 */
static GList *_folder_survey_styles_for_import()
{
  const dt_view_t *view = dt_view_manager_get_current_view(dt_view_manager_get_global());
  if(IS_NULL_PTR(view)) return NULL;

  char *conf = dt_conf_get_string(DT_FOLDER_SURVEY_STYLES_CONF_KEY);
  if(IS_NULL_PTR(conf) || conf[0] == '\0')
  {
    dt_free(conf);
    return NULL;
  }

  GList *styles = NULL;
  gchar **names = g_strsplit(conf, DT_FOLDER_SURVEY_STYLES_SEPARATOR, -1);
  for(gchar **name = names; *name; name++)
    if((*name)[0] != '\0') styles = g_list_prepend(styles, g_strdup(*name));

  g_strfreev(names);
  dt_free(conf);
  return g_list_reverse(styles);
}

/**
 * @brief Update one persisted entry when its asynchronous import completes.
 */
static void _folder_survey_imported(const char *source, const gboolean success, gpointer user_data)
{
  const guint generation = GPOINTER_TO_UINT(user_data);
  char *path = g_canonicalize_filename(source, NULL);

  dt_pthread_mutex_lock(&_folder_survey.lock);
  if(generation == _folder_survey.generation)
  {
    dt_folder_survey_entry_t *entry = g_hash_table_lookup(_folder_survey.files, path);
    if(!IS_NULL_PTR(entry))
    {
      entry->state = success ? DT_FOLDER_SURVEY_FILE_DONE : DT_FOLDER_SURVEY_FILE_PENDING;
      entry->stable_scans = success ? entry->stable_scans : 0;
      _folder_survey_save_locked();
    }
  }
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  dt_free(path);
}

/**
 * @brief Compare the current directory contents with the prior survey loop.
 *
 * New files are first recorded as pending. An unchanged size and modification
 * time on the following loop proves that the producer has stopped writing
 * before the import job receives the file.
 */
static int32_t _folder_survey_job_run(dt_job_t *job)
{
  dt_folder_survey_job_t *params = dt_control_job_get_params(job);
  GHashTable *observed = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, dt_free_gpointer);
  if(_folder_survey_collect(params->folder, observed))
  {
    g_hash_table_destroy(observed);
    return 1;
  }

  GList *imports = NULL;
  dt_pthread_mutex_lock(&_folder_survey.lock);
  if(_folder_survey.shutting_down || !_folder_survey.active || params->generation != _folder_survey.generation)
  {
    dt_pthread_mutex_unlock(&_folder_survey.lock);
    g_hash_table_destroy(observed);
    return 0;
  }

  GHashTableIter previous_iter;
  gpointer previous_path = NULL;
  gpointer previous_entry = NULL;
  g_hash_table_iter_init(&previous_iter, _folder_survey.files);
  // Forget paths removed since the preceding scan so they can be detected if they reappear.
  while(g_hash_table_iter_next(&previous_iter, &previous_path, &previous_entry))
  {
    if(!g_hash_table_contains(observed, previous_path)) g_hash_table_iter_remove(&previous_iter);
  }

  GHashTableIter observed_iter;
  gpointer observed_path = NULL;
  gpointer observed_value = NULL;
  g_hash_table_iter_init(&observed_iter, observed);
  // Compare each current image with its last size, timestamp, and import state.
  while(g_hash_table_iter_next(&observed_iter, &observed_path, &observed_value))
  {
    const dt_folder_survey_observation_t *observation = observed_value;
    dt_folder_survey_entry_t *entry = g_hash_table_lookup(_folder_survey.files, observed_path);

    if(!_folder_survey.baseline_initialized)
    {
      entry = calloc(1, sizeof(dt_folder_survey_entry_t));
      if(IS_NULL_PTR(entry)) continue;
      entry->size = observation->size;
      entry->mtime = observation->mtime;
      entry->state = DT_FOLDER_SURVEY_FILE_DONE;
      g_hash_table_replace(_folder_survey.files, g_strdup(observed_path), entry);
      continue;
    }

    if(IS_NULL_PTR(entry))
    {
      entry = calloc(1, sizeof(dt_folder_survey_entry_t));
      if(IS_NULL_PTR(entry)) continue;
      entry->size = observation->size;
      entry->mtime = observation->mtime;
      entry->state = DT_FOLDER_SURVEY_FILE_PENDING;
      g_hash_table_replace(_folder_survey.files, g_strdup(observed_path), entry);
      continue;
    }

    if(entry->state == DT_FOLDER_SURVEY_FILE_DONE)
    {
      // A producer may reuse a filename after the previous image was handled.
      // Treat changed metadata at the same path as a new pending file.
      if(entry->size != observation->size || entry->mtime != observation->mtime)
      {
        entry->size = observation->size;
        entry->mtime = observation->mtime;
        entry->stable_scans = 0;
        entry->state = DT_FOLDER_SURVEY_FILE_PENDING;
      }
      continue;
    }

    if(entry->state == DT_FOLDER_SURVEY_FILE_QUEUED) continue;

    if(entry->size != observation->size || entry->mtime != observation->mtime)
    {
      entry->size = observation->size;
      entry->mtime = observation->mtime;
      entry->stable_scans = 0;
      continue;
    }

    entry->stable_scans++;
    if(entry->stable_scans >= 1)
    {
      entry->state = DT_FOLDER_SURVEY_FILE_QUEUED;
      imports = g_list_prepend(imports, g_strdup(observed_path));
    }
  }

  _folder_survey.baseline_initialized = TRUE;
  _folder_survey_save_locked();
  dt_pthread_mutex_unlock(&_folder_survey.lock);
  g_hash_table_destroy(observed);

  if(!IS_NULL_PTR(imports))
  {
    imports = g_list_sort(imports, (GCompareFunc)g_strcmp0);
    const int elements = g_list_length(imports);
    dt_control_log(ngettext("Folder survey found %d new image to import.",
                            "Folder survey found %d new images to import.", elements),
                   elements);

    char *date = dt_conf_get_string("studio_capture/datetime");
    if(IS_NULL_PTR(date) || date[0] == '\0')
    {
      dt_free(date);
      GDateTime *now = g_date_time_new_now_local();
      date = g_date_time_format(now, "%F");
      g_date_time_unref(now);
    }

    dt_control_import_t data
        = { .imgs = imports,
            .datetime = dt_string_to_datetime(date),
            .copy = dt_conf_get_bool("studio_capture/copy"),
            .delete_source = dt_conf_get_bool("studio_capture/delete_source"),
            .folder_survey = TRUE,
            .on_conflict = CLAMP(dt_conf_get_int("studio_capture/on_conflict"), DT_IMPORT_ONCONFLICT_SKIP,
                                 DT_IMPORT_ONCONFLICT_UNIQUE),
            .styles = _folder_survey_styles_for_import(),
            .jobcode = dt_conf_get_string("studio_capture/jobcode"),
            .base_folder = dt_conf_get_string("studio_capture/base_directory_pattern"),
            .target_subfolder_pattern = dt_conf_get_string("studio_capture/sub_directory_pattern"),
            .target_file_pattern = dt_conf_get_string("studio_capture/filename_pattern"),
            .target_dir = NULL,
            .elements = elements,
            .discarded = NULL,
            .file_imported = _folder_survey_imported,
            .callback_data = GUINT_TO_POINTER(params->generation),
            .callback_data_free = NULL };
    dt_free(date);
    if(dt_control_import(data)) return 1;
  }

  return 0;
}

/**
 * @brief Release one scan job and allow the next periodic scan to start.
 */
static void _folder_survey_job_cleanup(void *data)
{
  dt_folder_survey_job_t *params = (dt_folder_survey_job_t *)data;
  dt_free(params->folder);
  dt_free(params);

  if(!_folder_survey.initialized) return;
  dt_pthread_mutex_lock(&_folder_survey.lock);
  _folder_survey.scan_running = FALSE;
  dt_pthread_mutex_unlock(&_folder_survey.lock);
}

/**
 * @brief Queue one background scan without overlapping the previous scan.
 */
static gboolean _folder_survey_scan(gpointer user_data)
{
  dt_pthread_mutex_lock(&_folder_survey.lock);
  if(_folder_survey.shutting_down || !_folder_survey.active || _folder_survey.scan_running)
  {
    dt_pthread_mutex_unlock(&_folder_survey.lock);
    return G_SOURCE_CONTINUE;
  }
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  char *folder = dt_conf_get_string("studio_capture/folder");
  if(IS_NULL_PTR(folder) || folder[0] == '\0' || !g_file_test(folder, G_FILE_TEST_IS_DIR))
  {
    dt_free(folder);
    return G_SOURCE_CONTINUE;
  }

  dt_pthread_mutex_lock(&_folder_survey.lock);
  if(_folder_survey.shutting_down || !_folder_survey.active || _folder_survey.scan_running)
  {
    dt_pthread_mutex_unlock(&_folder_survey.lock);
    dt_free(folder);
    return G_SOURCE_CONTINUE;
  }
  _folder_survey.scan_running = TRUE;
  dt_folder_survey_job_t *params = malloc(sizeof(dt_folder_survey_job_t));
  if(IS_NULL_PTR(params))
  {
    _folder_survey.scan_running = FALSE;
    dt_pthread_mutex_unlock(&_folder_survey.lock);
    dt_free(folder);
    return G_SOURCE_CONTINUE;
  }
  params->folder = folder;
  params->generation = _folder_survey.generation;
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  dt_job_t *job = dt_control_job_create(_folder_survey_job_run, "folder survey");
  if(IS_NULL_PTR(job))
  {
    _folder_survey_job_cleanup(params);
    return G_SOURCE_CONTINUE;
  }
  dt_control_job_set_params(job, params, _folder_survey_job_cleanup);
  dt_control_add_job(dt_control_get_global(), DT_JOB_QUEUE_SYSTEM_BG, job);
  return G_SOURCE_CONTINUE;
}

/**
 * @brief Run the immediate post-configuration scan only once.
 */
static gboolean _folder_survey_scan_once(gpointer user_data)
{
  _folder_survey.immediate_scan = 0;
  _folder_survey_scan(user_data);
  return G_SOURCE_REMOVE;
}

/**
 * @brief Recreate the periodic source after a frequency or state change.
 */
static void _folder_survey_reschedule()
{
  if(_folder_survey.timer > 0)
  {
    g_source_remove(_folder_survey.timer);
    _folder_survey.timer = 0;
  }
  if(_folder_survey.immediate_scan > 0)
  {
    g_source_remove(_folder_survey.immediate_scan);
    _folder_survey.immediate_scan = 0;
  }
  dt_pthread_mutex_lock(&_folder_survey.lock);
  const gboolean active = _folder_survey.active && !_folder_survey.shutting_down;
  dt_pthread_mutex_unlock(&_folder_survey.lock);
  if(!active) return;

  const int interval = CLAMP(dt_conf_get_int("studio_capture/interval"), 2, 3600);
  _folder_survey.timer = g_timeout_add_seconds(interval, _folder_survey_scan, NULL);
  _folder_survey.immediate_scan = g_idle_add(_folder_survey_scan_once, NULL);
}

/**
 * @brief Stop periodic scans without discarding the persisted comparison state.
 *
 * Imports already queued are allowed to finish. Starting again in the same
 * session resumes comparison from the last saved file list.
 */
static void _folder_survey_deactivate()
{
  if(_folder_survey.timer > 0)
  {
    g_source_remove(_folder_survey.timer);
    _folder_survey.timer = 0;
  }
  if(_folder_survey.immediate_scan > 0)
  {
    g_source_remove(_folder_survey.immediate_scan);
    _folder_survey.immediate_scan = 0;
  }

  dt_pthread_mutex_lock(&_folder_survey.lock);
  _folder_survey.active = FALSE;
  _folder_survey_save_locked();
  dt_pthread_mutex_unlock(&_folder_survey.lock);
}

char *dt_folder_survey_destination_preview()
{
  if(!dt_conf_get_bool("studio_capture/copy")) return NULL;

  char *folder = dt_conf_get_string("studio_capture/folder");
  char *base_folder = dt_conf_get_string("studio_capture/base_directory_pattern");
  char *subfolder = dt_conf_get_string("studio_capture/sub_directory_pattern");
  char *file_pattern = dt_conf_get_string("studio_capture/filename_pattern");
  char *date = dt_conf_get_string("studio_capture/datetime");
  char *path = NULL;

  const gboolean folder_valid
      = !IS_NULL_PTR(folder) && folder[0] && g_file_test(folder, G_FILE_TEST_IS_DIR);
  const gboolean base_valid = !IS_NULL_PTR(base_folder) && base_folder[0] && dt_util_test_writable_dir(base_folder);
  const gboolean patterns_valid = !IS_NULL_PTR(subfolder) && subfolder[0] != '\0' && !IS_NULL_PTR(file_pattern)
                                  && file_pattern[0] != '\0';

  if(folder_valid && base_valid && patterns_valid)
  {
    if(IS_NULL_PTR(date) || date[0] == '\0')
    {
      dt_free(date);
      GDateTime *now = g_date_time_new_now_local();
      date = g_date_time_format(now, "%F");
      g_date_time_unref(now);
    }

    char datetime_check[DT_DATETIME_LENGTH] = { 0 };
    dt_image_t *img = dt_alloc_align(sizeof(dt_image_t)); // dt_image_t is 64-aligned, see #1212
    if(dt_datetime_entry_to_exif(datetime_check, sizeof(datetime_check), date) && !IS_NULL_PTR(img))
    {
      dt_image_init(img);
      char *example = g_build_filename(folder, "example.raw", NULL);
      dt_control_import_t data = { .imgs = g_list_prepend(NULL, g_strdup(example)),
                                   .datetime = dt_string_to_datetime(date),
                                   .copy = TRUE,
                                   .folder_survey = TRUE,
                                   .jobcode = dt_conf_get_string("studio_capture/jobcode"),
                                   .base_folder = g_strdup(base_folder),
                                   .target_subfolder_pattern = g_strdup(subfolder),
                                   .target_file_pattern = g_strdup(file_pattern),
                                   .target_dir = NULL,
                                   .elements = 1,
                                   .discarded = NULL };
      path = dt_build_filename_from_pattern(example, 1, img, &data);

      // A valid destination must land inside the base directory.
      if(!IS_NULL_PTR(path) && path[0])
      {
        GFile *base = g_file_new_for_path(base_folder);
        GFile *destination = g_file_new_for_path(path);
        if(!g_file_has_prefix(destination, base))
        {
          dt_free(path);
          path = NULL;
        }
        else
        {
          // Full absolute paths get unwieldy in the UI preview: show only the base
          // directory's own name (not its whole ancestry) plus what's built under it,
          // e.g. ".../RAW Darktable/Scan/70 nouvel an/example.raw".
          char *relative = g_file_get_relative_path(base, destination);
          char *base_name = g_path_get_basename(base_folder);
          char *short_path = g_build_filename(base_name, relative, NULL);
          dt_free(path);
          path = g_strdup_printf(".../%s", short_path);
          dt_free(short_path);
          dt_free(base_name);
          dt_free(relative);
        }
        g_object_unref(base);
        g_object_unref(destination);
      }
      else
      {
        dt_free(path);
        path = NULL;
      }

      dt_free(example);
      dt_control_import_data_free(&data);
    }
    dt_free_align(img);
  }

  dt_free(folder);
  dt_free(base_folder);
  dt_free(subfolder);
  dt_free(file_pattern);
  dt_free(date);
  return path;
}

gboolean dt_folder_survey_can_start(const char **message)
{
  const char *folder = dt_conf_get_string_const("studio_capture/folder");
  if(IS_NULL_PTR(folder) || folder[0] == '\0' || !g_file_test(folder, G_FILE_TEST_IS_DIR))
  {
    *message = _("The folder to survey does not exist.");
    return FALSE;
  }
  if(g_access(folder, R_OK | X_OK) != 0)
  {
    *message = _("The folder to survey is not readable.");
    return FALSE;
  }

  const char *date = dt_conf_get_string_const("studio_capture/datetime");
  char datetime[DT_DATETIME_LENGTH] = { 0 };
  if(!IS_NULL_PTR(date) && date[0] && !dt_datetime_entry_to_exif(datetime, sizeof(datetime), date))
  {
    *message = _("The project date is invalid.");
    return FALSE;
  }

  if(!dt_conf_get_bool("studio_capture/copy")) return TRUE;

  const char *target = dt_conf_get_string_const("studio_capture/base_directory_pattern");
  if(IS_NULL_PTR(target) || target[0] == '\0' || !g_file_test(target, G_FILE_TEST_IS_DIR))
  {
    *message = _("The base directory does not exist.");
    return FALSE;
  }
  if(!dt_util_test_writable_dir(target))
  {
    *message = _("The base directory is not writable.");
    return FALSE;
  }

  const char *subfolder = dt_conf_get_string_const("studio_capture/sub_directory_pattern");
  if(IS_NULL_PTR(subfolder) || subfolder[0] == '\0')
  {
    *message = _("The project directory naming pattern is empty.");
    return FALSE;
  }
  const char *file_pattern = dt_conf_get_string_const("studio_capture/filename_pattern");
  if(IS_NULL_PTR(file_pattern) || file_pattern[0] == '\0')
  {
    *message = _("The file naming pattern is empty.");
    return FALSE;
  }

  char *canonical_folder = g_canonicalize_filename(folder, NULL);
  char *canonical_target = g_canonicalize_filename(target, NULL);
  GFile *source_file = g_file_new_for_path(canonical_folder);
  GFile *target_file = g_file_new_for_path(canonical_target);
  const gboolean target_inside_source
      = g_file_equal(source_file, target_file) || g_file_has_prefix(target_file, source_file);
  g_object_unref(source_file);
  g_object_unref(target_file);
  dt_free(canonical_folder);
  dt_free(canonical_target);
  if(target_inside_source)
  {
    *message = _("The base directory destination cannot be inside the surveyed folder.");
    return FALSE;
  }

  char *preview = dt_folder_survey_destination_preview();
  if(IS_NULL_PTR(preview))
  {
    *message = _("The configured destination path is invalid.");
    return FALSE;
  }
  dt_free(preview);

  return TRUE;
}

void dt_folder_survey_init()
{
  if(_folder_survey.initialized) return;

  dt_pthread_mutex_init(&_folder_survey.lock, NULL);
  _folder_survey.files = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, dt_free_gpointer);
  char config_dir[DT_PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(config_dir, sizeof(config_dir));
  _folder_survey.state_path = g_build_filename(config_dir, DT_FOLDER_SURVEY_STATE_FILE, NULL);
  _folder_survey.initialized = TRUE;

  dt_pthread_mutex_lock(&_folder_survey.lock);
  _folder_survey_load_locked();
  char *configured_folder = dt_conf_get_string("studio_capture/folder");
  char *canonical_folder = configured_folder && configured_folder[0]
                               ? g_canonicalize_filename(configured_folder, NULL)
                               : g_strdup("");
  if(g_strcmp0(_folder_survey.folder, canonical_folder))
  {
    g_hash_table_remove_all(_folder_survey.files);
    dt_free(_folder_survey.folder);
    _folder_survey.folder = g_strdup(canonical_folder);
    _folder_survey.baseline_initialized = FALSE;
    _folder_survey_save_locked();
  }
  dt_pthread_mutex_unlock(&_folder_survey.lock);
  dt_free(canonical_folder);
  dt_free(configured_folder);

  // One-time seed: Studio Capture's base directory is its own setting, kept
  // independent from the regular Import dialog past this point. But on its
  // very first use it has never been set, so adopt whatever the regular
  // Import dialog is currently configured with as a sensible starting value.
  char *base_dir = dt_conf_get_string("studio_capture/base_directory_pattern");
  if(IS_NULL_PTR(base_dir) || base_dir[0] == '\0')
  {
    char *default_base_dir = dt_conf_get_string("session/base_directory_pattern");
    if(!IS_NULL_PTR(default_base_dir) && default_base_dir[0])
      dt_conf_set_string("studio_capture/base_directory_pattern", default_base_dir);
    dt_free(default_base_dir);
  }
  dt_free(base_dir);
}

static dt_folder_survey_confirm_import_handler_t _confirm_import_handler = NULL;

void dt_folder_survey_set_confirm_import_handler(dt_folder_survey_confirm_import_handler_t handler)
{
  _confirm_import_handler = handler;
}

static void _folder_survey_offer_pending_import()
{
  const int new_files = dt_folder_survey_count_new_files();
  if(new_files <= 0) return;

  if(IS_NULL_PTR(_confirm_import_handler))
  {
    // Nobody registered to ask. Do NOT absorb: absorbing is the answer to a question that
    // was never put, and it would silently make these files invisible to every later scan.
    // Leaving the state untouched keeps them pending, to be offered whenever someone can ask.
    dt_print(DT_DEBUG_CONTROL,
             "[folder_survey] %d new file(s) pending, but no handler to ask about importing them\n",
             new_files);
    return;
  }

  if(!_confirm_import_handler(new_files))
  {
    // Declined: absorb every currently observed file into the baseline so a later scan does
    // not import it behind the user's back.
    dt_folder_survey_absorb_new_files();
    return;
  }

  // Accepted on a folder with no baseline yet: seed an initialized, empty one so the next
  // scan treats those files as new pending imports instead of silently absorbing them.
  dt_pthread_mutex_lock(&_folder_survey.lock);
  if(!_folder_survey.baseline_initialized)
  {
    _folder_survey.baseline_initialized = TRUE;
    _folder_survey_save_locked();
  }
  dt_pthread_mutex_unlock(&_folder_survey.lock);
}

int dt_folder_survey_start()
{
  if(!_folder_survey.initialized) dt_folder_survey_init();

  const char *message = NULL;
  if(!dt_folder_survey_can_start(&message))
  {
    dt_control_log("%s", message);
    return 1;
  }

  char *configured_folder = dt_conf_get_string("studio_capture/folder");
  char *canonical_folder = g_canonicalize_filename(configured_folder, NULL);
  dt_conf_set_string("studio_capture/folder", canonical_folder);

  dt_pthread_mutex_lock(&_folder_survey.lock);
  if(g_strcmp0(_folder_survey.folder, canonical_folder))
  {
    // A different source restarts the comparison from a fresh baseline.
    _folder_survey.generation++;
    g_hash_table_remove_all(_folder_survey.files);
    dt_free(_folder_survey.folder);
    _folder_survey.folder = g_strdup(canonical_folder);
    _folder_survey.baseline_initialized = FALSE;
    _folder_survey_save_locked();
  }
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  // Ask about images already in the folder before monitoring actually
  // starts: `active` is still FALSE at this point, so no scan can run
  // concurrently with this (blocking) prompt.
  _folder_survey_offer_pending_import();

  dt_pthread_mutex_lock(&_folder_survey.lock);
  _folder_survey.active = TRUE;
  _folder_survey_save_locked();
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  if(dt_conf_get_bool("studio_capture/copy"))
  {
    char *target = dt_conf_get_string("studio_capture/base_directory_pattern");
    dt_control_log(_("Folder survey started: `%s` to `%s`."), canonical_folder, target);
    dt_free(target);
  }
  else
    dt_control_log(_("Folder survey started: `%s`."), canonical_folder);

  dt_free(canonical_folder);
  dt_free(configured_folder);
  _folder_survey_reschedule();
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_FOLDER_SURVEY_CHANGED);
  return 0;
}

void dt_folder_survey_halt()
{
  if(!_folder_survey.initialized) return;

  dt_pthread_mutex_lock(&_folder_survey.lock);
  const gboolean active = _folder_survey.active;
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  _folder_survey_deactivate();
  if(active)
  {
    dt_control_log(_("Folder survey stopped."));
    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_FOLDER_SURVEY_CHANGED);
  }
}

gboolean dt_folder_survey_is_active()
{
  if(!_folder_survey.initialized) return FALSE;

  dt_pthread_mutex_lock(&_folder_survey.lock);
  const gboolean active = _folder_survey.active;
  dt_pthread_mutex_unlock(&_folder_survey.lock);
  return active;
}

gboolean dt_folder_survey_session_was_active()
{
  return _folder_survey.initialized && _folder_survey.was_active_last_session;
}

int dt_folder_survey_count_new_files()
{
  if(!_folder_survey.initialized) return 0;

  char *folder = dt_conf_get_string("studio_capture/folder");
  if(IS_NULL_PTR(folder) || folder[0] == '\0' || !g_file_test(folder, G_FILE_TEST_IS_DIR))
  {
    dt_free(folder);
    return 0;
  }

  GHashTable *observed = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, dt_free_gpointer);
  const int collect_error = _folder_survey_collect(folder, observed);
  dt_free(folder);
  if(collect_error)
  {
    g_hash_table_destroy(observed);
    return 0;
  }

  int count = 0;
  dt_pthread_mutex_lock(&_folder_survey.lock);
  GHashTableIter iter;
  gpointer path = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, observed);
  while(g_hash_table_iter_next(&iter, &path, &value))
  {
    // Without a baseline every observed file is a fresh candidate (a
    // never-before-surveyed folder has nothing to compare against yet).
    // With a baseline, only files not already marked done count (e.g. new
    // arrivals while the application was closed).
    const dt_folder_survey_entry_t *entry = g_hash_table_lookup(_folder_survey.files, path);
    if(!_folder_survey.baseline_initialized || IS_NULL_PTR(entry) || entry->state != DT_FOLDER_SURVEY_FILE_DONE)
      count++;
  }
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  g_hash_table_destroy(observed);
  return count;
}

void dt_folder_survey_absorb_new_files()
{
  if(!_folder_survey.initialized) return;

  char *folder = dt_conf_get_string("studio_capture/folder");
  if(IS_NULL_PTR(folder) || folder[0] == '\0' || !g_file_test(folder, G_FILE_TEST_IS_DIR))
  {
    dt_free(folder);
    return;
  }

  GHashTable *observed = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, dt_free_gpointer);
  const int collect_error = _folder_survey_collect(folder, observed);
  dt_free(folder);
  if(collect_error)
  {
    g_hash_table_destroy(observed);
    return;
  }

  dt_pthread_mutex_lock(&_folder_survey.lock);
  GHashTableIter iter;
  gpointer path = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&iter, observed);
  while(g_hash_table_iter_next(&iter, &path, &value))
  {
    const dt_folder_survey_observation_t *observation = value;
    dt_folder_survey_entry_t *entry = calloc(1, sizeof(dt_folder_survey_entry_t));
    if(IS_NULL_PTR(entry)) continue;
    entry->size = observation->size;
    entry->mtime = observation->mtime;
    entry->state = DT_FOLDER_SURVEY_FILE_DONE;
    g_hash_table_replace(_folder_survey.files, g_strdup(path), entry);
  }
  _folder_survey.baseline_initialized = TRUE;
  _folder_survey_save_locked();
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  g_hash_table_destroy(observed);
}

gboolean dt_folder_survey_take_session_marker()
{
  // Consumed, not merely read: the resume question must be asked at most once per start,
  // whatever the answer, so clearing here is the point rather than a side effect.
  if(!dt_folder_survey_session_was_active()) return FALSE;
  _folder_survey.was_active_last_session = FALSE;
  return TRUE;
}

void dt_folder_survey_forget_session()
{
  dt_pthread_mutex_lock(&_folder_survey.lock);
  _folder_survey_save_locked();
  dt_pthread_mutex_unlock(&_folder_survey.lock);
}

void dt_folder_survey_cleanup()
{
  if(!_folder_survey.initialized) return;

  dt_folder_survey_stop();

  dt_pthread_mutex_lock(&_folder_survey.lock);
  _folder_survey.generation++;
  _folder_survey_save_locked();
  dt_pthread_mutex_unlock(&_folder_survey.lock);

  g_hash_table_destroy(_folder_survey.files);
  _folder_survey.files = NULL;
  dt_free(_folder_survey.folder);
  dt_free(_folder_survey.state_path);
  dt_pthread_mutex_destroy(&_folder_survey.lock);
  _folder_survey.initialized = FALSE;
}

void dt_folder_survey_stop()
{
  if(!_folder_survey.initialized || _folder_survey.shutting_down) return;

  if(_folder_survey.timer > 0)
  {
    g_source_remove(_folder_survey.timer);
    _folder_survey.timer = 0;
  }
  if(_folder_survey.immediate_scan > 0)
  {
    g_source_remove(_folder_survey.immediate_scan);
    _folder_survey.immediate_scan = 0;
  }

  dt_pthread_mutex_lock(&_folder_survey.lock);
  // Application shutdown, NOT a user stop: persist the active flag as-is so an
  // interrupted session can be proposed for resume on the next start.
  // shutting_down gates every scan and import path from here on.
  _folder_survey.shutting_down = TRUE;
  _folder_survey_save_locked();
  dt_pthread_mutex_unlock(&_folder_survey.lock);
}
