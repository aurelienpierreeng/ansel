/*
    This file is part of the Ansel project.
    Copyright (C) 2025-2026 Aurélien PIERRE.
    
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
#include "system/mem_alloc.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "common/conf.h"
#include "import_jobs.h"
#include "common/collection.h"
#include "common/datetime.h"
#include "metadata/exif.h"
#include "develop/history_merge.h"
#include "metadata/metadata.h"
#include "common/styles.h"
#include "control/control.h"
#include "common/film.h"
#include "common/image.h"
#include "control/jobs/control_jobs.h"

#ifndef _WIN32
#endif
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include "common/utility.h"
#ifdef __APPLE__
#include "osx/osx.h"
#endif
#ifdef _WIN32
#endif


/**
 * @brief Creates folders from path.
 * Returns TRUE if success.
 *
 * @param path a valid folders path to create.
 * @return gboolean TRUE on error, FALSE on success
 */
gboolean _create_dir(const char *path)
{
  if(g_mkdir_with_parents(path, 0755) == -1)
  {
    fprintf(stderr, "failed to create directory %s.\n", path);
    dt_control_log(_("Impossible to create directory %s.\nThe target may be full or read-only.\n"), path);
    return TRUE;
  }
  return FALSE;
}

/**
 * @brief Replaces separator depending of the current OS
 * and removes whitespaces.
 *
 * @param path
 * @return gchar*
 */
gchar *_path_cleanup(gchar *path_in)
{
  gchar *clean = dt_cleanup_separators(path_in);
  gchar *path_out = dt_util_remove_whitespace(clean);
  dt_free(clean);
  return path_out;
}

gchar *dt_build_filename_from_pattern(const char *const filename, const int sequence, dt_image_t *img, dt_control_import_t *data)
{
  dt_variables_params_t *params;
  dt_variables_params_init(&params);
  // Borrowed references, like every other dt_variables_params_t caller (see iop/watermark.c):
  // dt_variables_params_destroy() does not own/free filename or jobcode.
  params->filename = filename;
  params->sequence = sequence;
  params->jobcode = data->jobcode;
  params->imgid = UNKNOWN_IMAGE;
  params->img = img;
  dt_variables_set_datetime(params, data->datetime);

  gchar *file_expand = dt_variables_expand(params, data->target_file_pattern, FALSE);
  gchar *path_expand = dt_variables_expand(params, data->target_subfolder_pattern, FALSE);

  // remove this if we decide to do the correction on user's settings directly
  gchar *file = _path_cleanup(file_expand);
  gchar *path = _path_cleanup(path_expand);
  dt_free(file_expand);
  dt_free(path_expand);

  gchar *dir = g_build_path(G_DIR_SEPARATOR_S, data->base_folder, path, (char *) NULL);
  data->target_dir = dt_util_normalize_path(dir);
  gchar *res = g_build_path(G_DIR_SEPARATOR_S, data->target_dir, file, (char *) NULL);

  dt_print(DT_DEBUG_PRINT, "[Import] Importing file to %s\n", res);

  dt_free(file);
  dt_free(path);
  dt_free(dir);
  dt_variables_params_destroy(params);
  return res;
}

/**
 * @brief Tests if file exist. Returns 1 if so.
 *
 * @param dest_file_path
 * @return gboolean
 */
gboolean _file_exist(const char *dest_file_path)
{
  return !IS_NULL_PTR(dest_file_path) && dest_file_path[0] && g_file_test(dest_file_path, G_FILE_TEST_EXISTS);
}

/**
 * @brief Just copy a file. Returns 1 if success.
 *
 * @param filename
 * @param dest_file_path
 * @return gboolean
 */
gboolean _copy_file(const char *filename, const char *dest_file_path)
{
  GFile *in = g_file_new_for_path(filename);
  GFile *out = g_file_new_for_path(dest_file_path);

  gboolean res = g_file_copy(in, out, G_FILE_COPY_NONE, 0, 0, 0, NULL);
  if(!res) dt_print(DT_DEBUG_IMPORT, "[Import] Could not copy the file %s to %s\n", filename, dest_file_path);

  g_object_unref(in);
  g_object_unref(out);

  return res;
}

/**
 * @brief Add an image entry in the database and returns its imgID
 *
 * @param data informations from the import module
 * @param img_path_to_db the file path to import
 * @return const int32_t
 */
const int32_t _import_job(dt_control_import_t *data, gchar *img_path_to_db)
{
  gchar *dirname = g_strdup(dt_util_path_get_dirname(img_path_to_db));

  dt_film_t film;
  const int32_t filmid = dt_film_new(&film, dirname);
  // Regular imports skip the per-file DT_SIGNAL_IMAGE_IMPORT (large batches
  // would otherwise raise it hundreds of times). Folder survey imports one or
  // a few files at a time, and Studio Capture needs that signal to know which
  // image to display as soon as it lands, so raise it for that case only.
  const int32_t imgid = dt_image_import(filmid, img_path_to_db, data->folder_survey);
  dt_free(dirname);
  return imgid;
}

/**
 * @brief Gets the computed xmp file name with apropriate number for import copy.
 * It computes a duplicate name based on the `counter` value.
 * The first path in the list ALWAYS become the default xmp.
 * So in case the default xmp was not found, the first one in the list is used as default.
 * Else, it's a duplicate.
 *
 * @param xmp_dest_name the destination name.
 * @param dest_file_path the full filename path of the destination image.
 * @param counter the number of duplicates.
 * @return void
 */
void dt_import_duplicate_get_dest_name(char *xmp_dest_name, const char *dest_file_path, const int counter)
{
  char *norm_dest_file = dt_util_normalize_path(dest_file_path);
  char *ext = norm_dest_file + safe_strlen(norm_dest_file);
  while(*ext != '.' && ext > norm_dest_file) ext--;
  const size_t name_len = safe_strlen(norm_dest_file) - safe_strlen(ext);

  if(counter == 0)
    g_snprintf(xmp_dest_name, DT_PATH_MAX, "%s.xmp", norm_dest_file);
  else
    g_snprintf(xmp_dest_name, DT_PATH_MAX, "%.*s_%.2d%s.xmp", (int)name_len, norm_dest_file, counter, ext);

  dt_print(DT_DEBUG_IMPORT, "[Import] XMP destination name: %s\n", xmp_dest_name);

  dt_free(norm_dest_file);
}

/**
 * @brief Attempt to find all sidecar XMP files along an image file and import (copy) it to destination.
 *
 * @param filename full path of original image file
 * @param dest_file_path full path of destination image file
 * @return int number of imported XMP
 */
int _import_copy_xmp(const char *const filename, gchar *dest_file_path)
{
  int xmp_cntr = 0;

  GList *xmp_files = dt_image_find_xmps(filename); // the first xmp will be the original
  if(g_list_length(xmp_files) > 0)
  {
    for(GList *current_xmp = xmp_files; current_xmp; current_xmp = g_list_next(current_xmp))
    {
      char *xmp_source = g_strdup((char*) current_xmp->data);
      gchar xmp_dest_name[DT_PATH_MAX] = { 0 };
      dt_import_duplicate_get_dest_name(xmp_dest_name, dest_file_path, xmp_cntr);

      // folder already created and writable, just copy.
      int success = _copy_file(xmp_source, xmp_dest_name);
      dt_print(DT_DEBUG_IMPORT, "[Import] copying %s to %s %s\n", xmp_source, xmp_dest_name,
               (success) ? "succeeded" : "failed");
      if(success) xmp_cntr++;
      dt_free(xmp_source);
    }
  }
  g_list_free(xmp_files);
  xmp_files = NULL;
  return xmp_cntr;
}

int _import_copy_txt(const char *const filename, const char *dest_file_path)
{
  char *txt_source = dt_image_get_text_path_from_path(filename);
  if(IS_NULL_PTR(txt_source)) return 0;

  char *txt_dest = dt_image_build_text_path_from_path(dest_file_path);
  int success = 0;

  if(!IS_NULL_PTR(txt_dest))
  {
    success = _copy_file(txt_source, txt_dest);
    dt_print(DT_DEBUG_IMPORT, "[Import] copying %s to %s %s\n", txt_source, txt_dest,
             (success) ? "succeeded" : "failed");
  }

  dt_free(txt_dest);
  dt_free(txt_source);
  return success ? 1 : 0;
}

/**
 * @brief copy a file to a destination path after checking if everything is allright.
 *
 * @param params job informations.
 * @param data import module information.
 * @param sequence 1-based number expanded by $(SEQUENCE), shared by all the files
 * of the same capture.
 * @param img_path_to_db will be set to the file path for import.
 * @param pathname_len the `img_path_to_db` size.
 * @param discarded the list of file pathes discarded because the target already exists
 * @return int -1 on copy error, 0 when the destination already existed, 1 when the file was copied
 */
int _import_copy_file(const char *const filename, const int sequence, dt_control_import_t *data, gchar *img_path_to_db, size_t pathname_len, GList **discarded)
{
  dt_image_t *img = dt_alloc_align(sizeof(dt_image_t));
  if(IS_NULL_PTR(img)) return -1;
  dt_image_init(img);

  if(strstr(data->target_file_pattern, "$(EXIF") != NULL
     || strstr(data->target_subfolder_pattern, "$(EXIF") != NULL)
  {
    dt_print(DT_DEBUG_IMPORT, "[Import] EXIF will be read for %s because the pattern needs it (performance penalty)\n", filename);
    dt_exif_read(img, filename);
  }

  gchar *dest_file_path = dt_build_filename_from_pattern(filename, sequence, img, data);
  dt_print(DT_DEBUG_IMPORT, "[Import] Image %s will be copied into %s\n", filename, dest_file_path);
  dt_free_align(img);

  gboolean exists = _file_exist(dest_file_path);

  if(exists && data->on_conflict == DT_IMPORT_ONCONFLICT_UNIQUE)
  {
    const char *dot = strrchr(dest_file_path, '.');
    const int stem_len = dot ? (int)(dot - dest_file_path) : (int)strlen(dest_file_path);
    const char *ext = dot ? dot : "";
    gchar *unique = NULL;
    for(int seq = 1; seq < 10000; seq++)
    {
      dt_free(unique);
      unique = g_strdup_printf("%.*s_%02d%s", stem_len, dest_file_path, seq, ext);
      if(!_file_exist(unique)) break;
    }
    dt_free(dest_file_path);
    dest_file_path = unique;
    exists = FALSE;
  }

  if(exists && data->on_conflict == DT_IMPORT_ONCONFLICT_OVERWRITE)
  {
    g_unlink(dest_file_path);
    exists = FALSE;
  }

  if(exists)
  {
    *discarded = g_list_prepend(*discarded, g_strdup(filename));
    g_strlcpy(img_path_to_db, dest_file_path, pathname_len);
    dt_print(DT_DEBUG_IMPORT, "[Import] File copy skipped, the target file %s already exists on the destination.\n", dest_file_path);
    dt_free(dest_file_path);
    return 0;
  }

  if(!dt_util_dir_exist(data->target_dir))
  {
    if(_create_dir(data->target_dir))
    {
      fprintf(stdout, "[Import] Unable to create the target folder %s.\n", data->target_dir);
      fprintf(stdout, "[Import] Not allowed to write in the %s folder.\n", data->target_dir);
      fprintf(stderr, "[Import] Unable to copy the file %s to %s.\n", filename, dest_file_path);
      dt_free(dest_file_path);
      return -1;
    }
  }
  else
    dt_print(DT_DEBUG_PRINT, "[Import] target folder %s already exists. Nothing to do.\n", data->target_dir);

  if(!dt_util_test_writable_dir(data->target_dir))
  {
    fprintf(stdout, "[Import] Not allowed to write in the %s folder.\n", data->target_dir);
    fprintf(stderr, "[Import] Unable to copy the file %s to %s.\n", filename, dest_file_path);
    dt_free(dest_file_path);
    return -1;
  }

  if(!_copy_file(filename, dest_file_path))
  {
    fprintf(stderr, "[Import] Unable to copy the file %s to %s.\n", filename, dest_file_path);
    dt_free(dest_file_path);
    return -1;
  }

  _import_copy_xmp(filename, dest_file_path);
  _import_copy_txt(filename, dest_file_path);
  g_strlcpy(img_path_to_db, dest_file_path, pathname_len);
  dt_free(dest_file_path);
  return 1;
}

void _write_xmp_id(const char *filename, int32_t imgid)
{
  GList *res = dt_metadata_get(imgid, "Xmp.darktable.image_id", NULL);
  if(!IS_NULL_PTR(res))
  {
    // Image ID is already set in metadata, don't overwrite it
    g_list_free_full(res, dt_free_gpointer);
    res = NULL;
    return;
  }
  // else : init it
  GError *error = NULL;
  GFile *gfile = g_file_new_for_path(filename);
  GFileInfo *info = g_file_query_info(gfile,
                            G_FILE_ATTRIBUTE_STANDARD_NAME ","
                            G_FILE_ATTRIBUTE_TIME_MODIFIED,
                            G_FILE_QUERY_INFO_NONE, NULL, &error);
  const char *fn = g_file_info_get_name(info);

  const time_t datetime = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  char dt_txt[DT_DATETIME_EXIF_LENGTH];
  dt_datetime_unix_to_exif(dt_txt, sizeof(dt_txt), &datetime);
  const char *id = g_strconcat(fn, "-", dt_txt, NULL);
  dt_metadata_set(imgid, "Xmp.darktable.image_id", id, FALSE);
  g_object_unref(info);
  g_object_unref(gfile);
  g_clear_error(&error);
}

/**
 * @brief Result of comparing two regular files byte-for-byte.
 */
typedef enum _file_compare_result_t
{
  FILES_COMPARE_IDENTICAL = 0,
  FILES_COMPARE_DIFFERENT,
  FILES_COMPARE_ERROR
} _file_compare_result_t;

/**
 * @brief Compare two regular files byte-for-byte.
 *
 * @param source path to the first file.
 * @param destination path to the second file.
 * @return FILES_COMPARE_IDENTICAL if both files exist, have the same size, and identical contents;
 *         FILES_COMPARE_DIFFERENT if sizes or contents differ;
 *         FILES_COMPARE_ERROR on any stat/open/allocation/read error.
 */
static _file_compare_result_t _compare_files(const char *source, const char *destination)
{
  FILE *source_file = g_fopen(source, "rb");
  FILE *destination_file = g_fopen(destination, "rb");
  if(IS_NULL_PTR(source_file) || IS_NULL_PTR(destination_file))
  {
    if(!IS_NULL_PTR(source_file)) fclose(source_file);
    if(!IS_NULL_PTR(destination_file)) fclose(destination_file);
    return FILES_COMPARE_ERROR;
  }

  GStatBuf source_stat;
  GStatBuf destination_stat;
  if(fstat(fileno(source_file), &source_stat) != 0
     || fstat(fileno(destination_file), &destination_stat) != 0)
  {
    fclose(source_file);
    fclose(destination_file);
    return FILES_COMPARE_ERROR;
  }

  if(source_stat.st_size != destination_stat.st_size)
  {
    fclose(source_file);
    fclose(destination_file);
    return FILES_COMPARE_DIFFERENT;
  }

  const size_t buffer_size = 64 * 1024;
  unsigned char *source_buffer = dt_alloc_align(buffer_size);
  unsigned char *destination_buffer = dt_alloc_align(buffer_size);
  if(IS_NULL_PTR(source_buffer) || IS_NULL_PTR(destination_buffer))
  {
    dt_free_align(source_buffer);
    dt_free_align(destination_buffer);
    fclose(source_file);
    fclose(destination_file);
    return FILES_COMPARE_ERROR;
  }

  _file_compare_result_t result = FILES_COMPARE_IDENTICAL;
  while(result == FILES_COMPARE_IDENTICAL)
  {
    const size_t source_read = fread(source_buffer, 1, buffer_size, source_file);
    const size_t destination_read = fread(destination_buffer, 1, buffer_size, destination_file);

    if(ferror(source_file) || ferror(destination_file))
      result = FILES_COMPARE_ERROR;
    else if(source_read != destination_read || memcmp(source_buffer, destination_buffer, source_read))
      result = FILES_COMPARE_DIFFERENT;

    if(source_read < buffer_size) break;
  }

  dt_free_align(source_buffer);
  dt_free_align(destination_buffer);
  fclose(source_file);
  fclose(destination_file);

  return result;
}

/**
 * @brief process to copy (or not) and import an image to database.
 *
 * @param img the current image.
 * @param data info from import module.
 * @param sequence 1-based number expanded by $(SEQUENCE), shared by all the files
 * of the same capture.
 * @return int32_t the imgid of the imported image (or -1 if import failed)
 */
int32_t _import_image(const GList *img, dt_control_import_t *data, const int sequence, GList **discarded, int *xmps)
{
  const char *filename = (const char*) img->data;
  gchar img_path_to_db[DT_PATH_MAX] = { 0 };
  int copy_status = 0;

  if(data->copy)
  {
    copy_status = _import_copy_file(filename, sequence, data, img_path_to_db, sizeof(img_path_to_db), discarded);
    if(copy_status < 0) return UNKNOWN_IMAGE;
  }
  else
    g_strlcpy(img_path_to_db, filename, sizeof(img_path_to_db));

  if(img_path_to_db[0] == '\0')
  {
    fprintf(stderr, "[Import] Could not import file from disk: empty file path\n");
    return UNKNOWN_IMAGE;
  }

  int32_t imgid = _import_job(data, img_path_to_db);
  if(imgid == UNKNOWN_IMAGE)
  {
    dt_control_log(_("Error importing file in collection: %s"), img_path_to_db);
    fprintf(stderr, "[Import] Error importing file in collection: %s", img_path_to_db);
    return UNKNOWN_IMAGE;
  }

  *xmps = dt_image_read_duplicates(imgid, img_path_to_db, FALSE);
  dt_print(DT_DEBUG_IMPORT, "[Import] Found and imported %i XMP for %s.\n", *xmps, img_path_to_db);
  dt_print(DT_DEBUG_IMPORT, "[Import] successfully imported %s in DB at imgid %i\n", img_path_to_db, imgid);

  if(data->delete_source && copy_status == 1)
  {
    const _file_compare_result_t comparison = _compare_files(filename, img_path_to_db);
    if(comparison == FILES_COMPARE_IDENTICAL)
    {
      if(g_unlink(filename) != 0)
        dt_control_log(_("The imported file was verified but the original could not be deleted: %s"), filename);
    }
    else if(comparison == FILES_COMPARE_DIFFERENT)
    {
      dt_control_log(_("The imported file differs from the original, which was not deleted: %s"), filename);
    }
    else
    {
      dt_control_log(_("The imported file could not be verified against the original, which was not deleted: %s"), filename);
    }
  }

  if(!IS_NULL_PTR(data->styles))
  {
    dt_hm_batch_state_t batch = { 0 };
    for(GList *s = data->styles; s; s = g_list_next(s))
    {
      const char *style_name = (const char *)s->data;
      const int32_t style_id = dt_styles_get_id_by_name(style_name);
      if(style_id <= 0) continue;
      dt_styles_apply_to_image_merge(style_name, style_id, imgid, DT_HISTORY_MERGE_APPEND, &batch);
    }
    dt_hm_batch_state_cleanup(&batch);
    dt_image_history_changed(imgid, TRUE);
  }

  return imgid;
}

/**
 * @brief Assign a 1-based sequence number shared by all files of the same capture.
 *
 * The capture key is the source path with the last extension removed, but only when
 * that extension lies after the last directory separator. A fresh key is inserted into
 * @p capture_sequences on the first encounter and owned by the table; on subsequent
 * encounters the local copy is freed and the existing number is reused.
 *
 * @param capture_sequences Hash table mapping capture keys to sequence numbers.
 * @param filename Full path of the source file.
 * @param[in,out] sequence Last sequence number assigned; incremented when a new capture is seen.
 * @return int The sequence number for @p filename, 1-based.
 */
int dt_control_import_capture_sequence(GHashTable *capture_sequences, const char *filename, int *sequence)
{
  gchar *capture = g_strdup(filename);
  gchar *dot = strrchr(capture, '.');
  const gchar *sep = strrchr(capture, G_DIR_SEPARATOR);
  const gchar *sep_alt = strrchr(capture, G_DIR_SEPARATOR == '/' ? '\\' : '/');
  if(!IS_NULL_PTR(sep_alt) && (IS_NULL_PTR(sep) || sep_alt > sep)) sep = sep_alt;
  if(!IS_NULL_PTR(dot) && (IS_NULL_PTR(sep) || dot > sep)) *dot = '\0';

  int file_sequence;
  const gpointer known_sequence = g_hash_table_lookup(capture_sequences, capture);
  if(!IS_NULL_PTR(known_sequence))
  {
    dt_free(capture);
    file_sequence = GPOINTER_TO_INT(known_sequence);
  }
  else
  {
    (*sequence)++;
    g_hash_table_replace(capture_sequences, capture, GINT_TO_POINTER(*sequence));
    file_sequence = *sequence;
  }
  return file_sequence;
}

/**
 * @brief Update import progress with wording matching the import origin.
 */
static void _refresh_progress_counter(dt_job_t *job, const int elements, const int index,
                                      const gboolean folder_survey)
{
  gchar message[128] = { 0 };
  double fraction = (double)index / (double)elements;
  if(folder_survey)
    snprintf(message, sizeof(message),
             ngettext("Capture: importing %i/%i image", "Capture: importing %i/%i images", index),
             index, elements);
  else
    snprintf(message, sizeof(message),
             ngettext("Importing %i/%i image", "Importing %i/%i images", index),
             index, elements);
  dt_control_job_set_progress_message(job, message);
  dt_control_job_set_progress(job, fraction);
  g_usleep(100);
}

static int32_t _control_import_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  dt_control_import_t *data = params->data;

  int index = 0;
  int xmps = 0;
  int32_t last_imgid = UNKNOWN_IMAGE;
  gint64 last_collection_refresh = 0;

  GHashTable *capture_sequences = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  int sequence = 0;

  const dt_collection_import_view_t first_image_policy = data->folder_survey
                                                         ? DT_COLLECTION_IMPORT_VIEW_KEEP
                                                         : DT_COLLECTION_IMPORT_VIEW_GRID;
  const dt_collection_import_view_t single_image_policy = data->folder_survey
                                                          ? DT_COLLECTION_IMPORT_VIEW_KEEP
                                                          : DT_COLLECTION_IMPORT_VIEW_IMAGE;

  for(GList *img = g_list_first(data->imgs); img; img = g_list_next(img))
  {
    dt_print(DT_DEBUG_IMPORT, "[Import] starting import of image #%i...\n", index);
    _refresh_progress_counter(job, data->elements, index, data->folder_survey);

    const int file_sequence = dt_control_import_capture_sequence(capture_sequences,
                                                                  (const char *)img->data,
                                                                  &sequence);

    const int32_t current_imgid = _import_image(img, data, file_sequence, &data->discarded, &xmps);
    if(!IS_NULL_PTR(data->file_imported))
      data->file_imported((const char *)img->data, current_imgid > UNKNOWN_IMAGE, data->callback_data);

    if(current_imgid <= UNKNOWN_IMAGE) continue;

    if(index == 0)
      dt_collection_load_filmroll(dt_collection_get_global(), current_imgid, first_image_policy, TRUE);

    dt_collection_notify_imported(current_imgid, NULL, &last_collection_refresh);
    last_imgid = current_imgid;
    index++;
  }
  g_hash_table_destroy(capture_sequences);

  if(index > 0)
    dt_collection_update_query(dt_collection_get_global(), DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);

  dt_conf_set_int("ui_last/nb_imported", index);

  if(index == 0)
  {
    dt_control_log(data->folder_survey ? _("Capture: No image imported!") : _("No image imported!"));
    fprintf(stderr, "No image imported!\n\n");
    return 1;
  }

  if(index == 1 && xmps <= 1)
  {
    if(data->folder_survey) dt_control_log(_("Capture: imported 1 image."));
    dt_collection_load_filmroll(dt_collection_get_global(), last_imgid, single_image_policy, TRUE);
    return 0;
  }

  dt_control_log(data->folder_survey
                 ? ngettext("Capture: imported %d image.", "Capture: imported %d images.", index)
                 : ngettext("Imported %d image", "Imported %d images", index), index);
  fprintf(stdout, "%d files imported in database.\n\n", index);

  return 0;
}

void dt_control_import_data_free(dt_control_import_t *data)
{
  g_date_time_unref(data->datetime);
  dt_free(data->jobcode);
  dt_free(data->base_folder);
  dt_free(data->target_subfolder_pattern);
  dt_free(data->target_file_pattern);
  dt_free(data->target_dir);
  if(data->styles)
  {
    g_list_free_full(data->styles, dt_free_gpointer);
    data->styles = NULL;
  }
  if(!IS_NULL_PTR(data->callback_data_free))
    data->callback_data_free(data->callback_data);
  data->callback_data = NULL;

  // GList of pathes stored as *char. We need to free the list and the *char
  if(data->discarded)
  {
    g_list_free_full(data->discarded, dt_free_gpointer);
    data->discarded = NULL;
  }
  if(data->imgs)
  {
    g_list_free_full(data->imgs, dt_free_gpointer);
    data->imgs = NULL;
  }
}

/* Installed by the GUI at startup; absent under ansel-cli, where the discarded list is
 * simply freed with the rest -- a headless import has nobody to show a dialog to. */
static dt_control_import_discarded_handler_t _discarded_files_handler = NULL;

void dt_control_import_set_discarded_files_handler(dt_control_import_discarded_handler_t handler)
{
  _discarded_files_handler = handler;
}

/* The discarded-files recap dialog lives in gui/import.c now: it was the only GUI
 * code in this job file, and the only reason a control-layer TU included
 * widgets/ and gui/ headers. The job reaches it through the handler below. */

static void _control_import_job_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)p;
  dt_control_import_t *data = params->data;

  // Display a recap of files that weren't copied
  if(g_list_length(data->discarded) > 0 && _discarded_files_handler)
  {
    // Called on THIS worker thread: getting onto the GUI main loop is the handler's
    // business (g_main_context_invoke is GTK machinery, not a job's). The handler owns
    // freeing data and params, whenever it is done with them.
    _discarded_files_handler((struct dt_control_image_enumerator_t *)params);
  }
  else
  {
    dt_control_import_data_free(data);
    dt_free(data);
    dt_control_image_enumerator_cleanup(params);
  }
}

static void *_control_import_alloc()
{
  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(IS_NULL_PTR(params)) return NULL;

  params->data = g_malloc0(sizeof(dt_control_import_t));
  if(IS_NULL_PTR(params->data))
  {
    dt_control_image_enumerator_cleanup(params);
    return NULL;
  }
  return params;
}

static dt_job_t *_control_import_job_create(dt_control_import_t data)
{
  dt_job_t *job = dt_control_job_create(&_control_import_job_run, "import");
  if(IS_NULL_PTR(job)) return NULL;
  dt_control_image_enumerator_t *params = _control_import_alloc();
  if(IS_NULL_PTR(params))
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  memcpy(params->data, &data, sizeof(dt_control_import_t));
  params->index = NULL;
  dt_control_job_add_progress(job, _("import"), FALSE);
  dt_control_job_set_params(job, params, _control_import_job_cleanup);
  return job;
}

int dt_control_import(dt_control_import_t data)
{
  dt_job_t *job = _control_import_job_create(data);
  if(IS_NULL_PTR(job))
  {
    // Report every source as failed so asynchronous clients can release their queued state.
    for(GList *img = g_list_first(data.imgs); img; img = g_list_next(img))
      if(!IS_NULL_PTR(data.file_imported))
        data.file_imported((const char *)img->data, FALSE, data.callback_data);

    dt_control_import_data_free(&data);
    return 1;
  }

  return dt_control_add_job(dt_control_get_global(), DT_JOB_QUEUE_USER_FG, job);
}
