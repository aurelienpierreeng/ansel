#include "import_jobs.h"
#include "common/collection.h"
#include "common/datetime.h"
#include "common/exif.h"
#include "common/metadata.h"
#include "control/control.h"
#include "common/image.h"
#include "control/jobs/control_jobs.h"

#ifndef _WIN32
#include <glob.h>
#endif
#include <string.h>
#ifdef __APPLE__
#include "osx/osx.h"
#endif
#ifdef _WIN32
#include "win/dtwin.h"
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
  g_free(clean);
  return path_out;
}

gchar *dt_build_filename_from_pattern(const char *const filename, const int index, dt_image_t *img, dt_control_import_t *data)
{
  dt_variables_params_t *params;
  dt_variables_params_init(&params);
  params->filename = g_strdup(filename);
  params->sequence = index;
  params->jobcode = g_strdup(data->jobcode);
  params->imgid = UNKNOWN_IMAGE;
  params->img = img;
  dt_variables_set_datetime(params, data->datetime);

  gchar *file_expand = dt_variables_expand(params, data->target_file_pattern, FALSE);
  gchar *path_expand = dt_variables_expand(params, data->target_subfolder_pattern, FALSE);

  // remove this if we decide to do the correction on user's settings directly
  gchar *file = _path_cleanup(file_expand);
  gchar *path = _path_cleanup(path_expand);
  g_free(file_expand);
  g_free(path_expand);

  gchar *dir = g_build_path(G_DIR_SEPARATOR_S, data->base_folder, path, (char *) NULL);
  data->target_dir = dt_util_normalize_path(dir);
  gchar *res = g_build_path(G_DIR_SEPARATOR_S, data->target_dir, file, (char *) NULL);

  dt_print(DT_DEBUG_PRINT, "[Import] Importing file to %s\n", res);

  g_free(file);
  g_free(path);
  g_free(dir);
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
  return dest_file_path != NULL && dest_file_path[0] && g_file_test(dest_file_path, G_FILE_TEST_EXISTS);
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
  const int32_t imgid = dt_image_import(filmid, img_path_to_db, FALSE);
  g_free(dirname);
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
    g_snprintf(xmp_dest_name, PATH_MAX, "%s.xmp", norm_dest_file);
  else
    g_snprintf(xmp_dest_name, PATH_MAX, "%.*s_%.2d%s.xmp", (int)name_len, norm_dest_file, counter, ext);

  dt_print(DT_DEBUG_IMPORT, "[Import] XMP destination name: %s\n", xmp_dest_name);

  free(norm_dest_file);
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
      gchar xmp_dest_name[PATH_MAX] = { 0 };
      dt_import_duplicate_get_dest_name(xmp_dest_name, dest_file_path, xmp_cntr);

      // folder already created and writable, just copy.
      int success = _copy_file(xmp_source, xmp_dest_name);
      dt_print(DT_DEBUG_IMPORT, "[Import] copying %s to %s %s\n", xmp_source, xmp_dest_name,
               (success) ? "succeeded" : "failed");
      if(success) xmp_cntr++;
      free(xmp_source);
    }
  }
  g_list_free(xmp_files);
  return xmp_cntr;
}

int _import_copy_txt(const char *const filename, const char *dest_file_path)
{
  char *txt_source = dt_image_get_text_path_from_path(filename);
  if(!txt_source) return 0;

  char *txt_dest = dt_image_build_text_path_from_path(dest_file_path);
  int success = 0;

  if(txt_dest)
  {
    success = _copy_file(txt_source, txt_dest);
    dt_print(DT_DEBUG_IMPORT, "[Import] copying %s to %s %s\n", txt_source, txt_dest,
             (success) ? "succeeded" : "failed");
  }

  g_free(txt_dest);
  g_free(txt_source);
  return success ? 1 : 0;
}

/**
 * @brief copy a file to a destination path after checking if everything is allright.
 *
 * @param params job informations.
 * @param data import module information.
 * @param img_path_to_db will be set to the file path for import.
 * @param pathname_len the `img_path_to_db` size.
 * @param discarded the list of file pathes discarded because the target already exists
 * @return int
 */
int _import_copy_file(const char *const filename, const int index, dt_control_import_t *data, gchar *img_path_to_db, size_t pathname_len, GList **discarded)
{
  dt_image_t *img = malloc(sizeof(dt_image_t));
  dt_image_init(img);

  // Generate file I/O only if the pattern is using EXIF variables.
  // Otherwise, discard it since it's really expensive if the file is on external/remote storage.
  // This is mandatory BEFORE expanding variables in pattern
  if(strstr(data->target_file_pattern, "$(EXIF") != NULL
    || strstr(data->target_subfolder_pattern, "$(EXIF") != NULL )
  {
    dt_print(DT_DEBUG_IMPORT, "[Import] EXIF will be read for %s because the pattern needs it (performance penalty)\n", filename);
    dt_exif_read(img, filename);
  }

  gchar *dest_file_path = dt_build_filename_from_pattern(filename, index, img, data);
  dt_print(DT_DEBUG_IMPORT, "[Import] Image %s will be copied into %s\n", filename, dest_file_path);
  free(img);

  int process = TRUE;

  if(!_file_exist(dest_file_path))
  {
    if(!dt_util_dir_exist(data->target_dir))
      process = !_create_dir(data->target_dir);
    else
      dt_print(DT_DEBUG_PRINT, "[Import] target folder %s already exists. Nothing to do.\n", data->target_dir);

    if(process)
      process = dt_util_test_writable_dir(data->target_dir);
    else
      fprintf(stdout, "[Import] Unable to create the target folder %s.\n", data->target_dir);

    if(process)
      process = _copy_file(filename, dest_file_path);
    else
      fprintf(stdout, "[Import] Not allowed to write in the %s folder.\n", data->target_dir);

    if(process)
    {
      _import_copy_xmp(filename, dest_file_path);
      _import_copy_txt(filename, dest_file_path);
    }

    if(process)
      g_strlcpy(img_path_to_db, dest_file_path, pathname_len);
    else
      fprintf(stderr, "[Import] Unable to copy the file %s to %s.\n", img_path_to_db, dest_file_path);
  }
  else
  {
    *discarded = g_list_prepend(*discarded, g_strdup(filename));
    g_strlcpy(img_path_to_db, dest_file_path, pathname_len);
    dt_print(DT_DEBUG_IMPORT, "[Import] File copy skipped, the target file %s already exists on the destination.\n", dest_file_path);
  }

  g_free(dest_file_path);
  return !process;
}

void _write_xmp_id(const char *filename, int32_t imgid)
{
  GList *res = dt_metadata_get(imgid, "Xmp.darktable.image_id", NULL);
  if(res != NULL)
  {
    // Image ID is already set in metadata, don't overwrite it
    g_list_free_full(res, g_free);
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
 * @brief process to copy (or not) and import an image to database.
 *
 * @param img the current image.
 * @param data info from import module.
 * @param index current loop's index.
 * @return int32_t the imgid of the imported image (or -1 if import failed)
 */
int32_t _import_image(const GList *img, dt_control_import_t *data, const int index, GList **discarded, int *xmps)
{
  const char *filename = (const char*) img->data;

  gchar img_path_to_db[PATH_MAX] = { 0 };
  gboolean process_error = FALSE;
  int32_t imgid = UNKNOWN_IMAGE;

  if(data->copy)
    // Copy the file to destination folder, expanding variables internally
    process_error = _import_copy_file(filename, index, data, img_path_to_db, sizeof(img_path_to_db), discarded);
  else
    // destination = origin, nothing to do
    g_strlcpy(img_path_to_db, filename, sizeof(img_path_to_db));

  if(process_error)
    ;
  else if(img_path_to_db[0] == 0)
    fprintf(stderr, "[Import] Could not import file from disk: empty file path\n");
  else
  {
    imgid = _import_job(data, img_path_to_db);

    if(imgid == UNKNOWN_IMAGE)
    {
      dt_control_log(_("Error importing file in collection: %s"), img_path_to_db);
      fprintf(stderr, "[Import] Error importing file in collection: %s", img_path_to_db);
    }
    else
    {
      // read all sidecar files (including the original one) and import them if not found in db.
      *xmps = dt_image_read_duplicates(imgid, img_path_to_db, FALSE);
      dt_print(DT_DEBUG_IMPORT, "[Import] Found and imported %i XMP for %s.\n", *xmps, img_path_to_db);
      dt_print(DT_DEBUG_IMPORT, "[Import] successfully imported %s in DB at imgid %i\n", img_path_to_db, imgid);
    }
  }

  return imgid;
}

void _refresh_progress_counter(dt_job_t *job, const int elements, const int index)
{
  gchar message[32] = { 0 };
  double fraction = (double)index / (double)elements;
  snprintf(message, sizeof(message), ngettext("importing %i/%i image", "importing %i/%i images", index), index, elements);
  dt_control_job_set_progress_message(job, message);
  dt_control_job_set_progress(job, fraction);
  g_usleep(100);
}

static int32_t _control_import_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)dt_control_job_get_params(job);
  dt_control_import_t *data = params->data;

  int index = 0;
  int xmps = 0; // number of xmps imported in db.
  int32_t imgid = UNKNOWN_IMAGE;

  for(GList *img = g_list_first(data->imgs); img; img = g_list_next(img))
  {
    dt_print(DT_DEBUG_IMPORT, "[Import] starting import of image #%i...\n", index);

    _refresh_progress_counter(job, data->elements, index);
    imgid = _import_image(img, data, index, &data->discarded, &xmps);

    if(imgid > UNKNOWN_IMAGE)
    {
      // On first image, we change the current filmroll in collection.
      // On the next, we simply update the collection.
      if(index == 0)
        dt_collection_load_filmroll(darktable.collection, imgid, FALSE);
      else
        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);

      index++;
    }
  }

  if(index == 0)
  {
    dt_control_log(_("No image imported!"));
    fprintf(stderr, "No image imported!\n\n");
  }
  // don't open picture in darkroom if more than 1 xmps (= duplicates) have been imported.
  else if(index == 1 && xmps == 1)
  {
    dt_collection_load_filmroll(darktable.collection, imgid, TRUE);
  }
  else
  {
    dt_control_log(ngettext("imported %d image", "imported %d images", index), index);
    fprintf(stdout, "%d files imported in database.\n\n", index);
  }

  dt_conf_set_int("ui_last/nb_imported", index);

  return index >= 1 ? 0 : 1;
}

void dt_control_import_data_free(dt_control_import_t *data)
{
  g_date_time_unref(data->datetime);
  g_free(data->jobcode);
  g_free(data->base_folder);
  g_free(data->target_subfolder_pattern);
  g_free(data->target_file_pattern);
  g_free(data->target_dir);

  // GList of pathes stored as *char. We need to free the list and the *char
  if(data->discarded) g_list_free_full(data->discarded, g_free);
  if(data->imgs) g_list_free_full(data->imgs, g_free);
}

static int _discarded_files_popup(dt_control_image_enumerator_t *params)
{
  dt_control_import_t *data = params->data;

  // Create the window
  GtkWidget *dialog = gtk_dialog_new_with_buttons("Message",
    GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
    GTK_DIALOG_DESTROY_WITH_PARENT,
    _("_OK"),
    GTK_RESPONSE_NONE,
    NULL);
  gtk_window_set_title(GTK_WINDOW(dialog), _("Some files have not been copied"));
  gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(800), DT_PIXEL_APPLY_DPI(800));

  // Create the label
  GtkWidget *label = gtk_label_new(_("The following source files have not been copied "
    "because similarly-named files already exist on the destination. "
    "This may be because the files have already been imported "
    "or the naming pattern leads to non-unique file names."));
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

  // Create the scrolled window internal container
  GtkWidget *scrolled_window = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC,
  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled_window), TRUE);

  // Create the treeview model from the list of discarded file pathes
  GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);
  GtkTreeIter iter;
  for(GList *file = g_list_first(data->discarded); file; file = g_list_next(file))
  {
    if(file->data)
    {
      gtk_list_store_append(store, &iter);
      gtk_list_store_set(store, &iter, 0, (char *)file->data, -1);
    }
  }

  // Create the treeview view. Sooooo verbose... it's only a flat list.
  GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(col, _("Origin path"));
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_attributes(col, renderer, "text", 0, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
  g_object_unref(store);

  // Pack widgets to an unified box
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), scrolled_window, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(scrolled_window), view);

  // Pack the box to the dialog internal container
  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_add(GTK_CONTAINER(content_area), box);
  gtk_widget_show_all(dialog);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  dt_control_import_data_free(data);
  free(data);
  dt_control_image_enumerator_cleanup(params);

  return 0;
}

static void _control_import_job_cleanup(void *p)
{
  dt_control_image_enumerator_t *params = (dt_control_image_enumerator_t *)p;
  dt_control_import_t *data = params->data;

  // Display a recap of files that weren't copied
  if(g_list_length(data->discarded) > 0)
  {
    g_main_context_invoke(NULL, (GSourceFunc)_discarded_files_popup, (gpointer)params);
    // we will free data and params from the function since it's run asynchronously
  }
  else
  {
    dt_control_import_data_free(data);
    free(data);
    dt_control_image_enumerator_cleanup(params);
  }
}

static void *_control_import_alloc()
{
  dt_control_image_enumerator_t *params = dt_control_image_enumerator_alloc();
  if(!params) return NULL;

  params->data = g_malloc0(sizeof(dt_control_import_t));
  if(!params->data)
  {
    _control_import_job_cleanup(params);
    return NULL;
  }
  return params;
}

static dt_job_t *_control_import_job_create(dt_control_import_t data)
{
  dt_job_t *job = dt_control_job_create(&_control_import_job_run, "import");
  if(!job) return NULL;
  dt_control_image_enumerator_t *params = _control_import_alloc();
  if(!params)
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

void dt_control_import(dt_control_import_t data)
{
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, _control_import_job_create(data));
}
