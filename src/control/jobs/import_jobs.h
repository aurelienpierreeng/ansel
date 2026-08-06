/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Aurélien PIERRE.
    
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
#ifndef DT_CONTROL_JOBS_IMPORT_JOBS_H
#define DT_CONTROL_JOBS_IMPORT_JOBS_H

#include "common/image.h"
#include "common/variables.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Behaviour when a copy-on-import destination path already exists.
 *
 * SKIP is value 0 so the default (zero-initialized) behaviour of every existing
 * caller is preserved: the existing destination is kept and imported as-is.
 */
typedef enum dt_import_onconflict_t
{
  DT_IMPORT_ONCONFLICT_SKIP = 0,      // keep the existing destination file (legacy behaviour)
  DT_IMPORT_ONCONFLICT_OVERWRITE = 1, // replace the existing destination with the source
  DT_IMPORT_ONCONFLICT_UNIQUE = 2     // copy the source under a new, non-colliding name
} dt_import_onconflict_t;

typedef struct dt_control_import_t
{
  GList *imgs;
  GDateTime *datetime;
  gboolean copy;
  gboolean delete_source;
  gboolean folder_survey;

  // Behaviour when a copy destination path already exists.
  dt_import_onconflict_t on_conflict;

  // Optional list of style names (owned char*) applied, in order, to each
  // successfully imported image. All styles are applied in APPEND mode.
  GList *styles;

  // String expanded as $(JOBCODE) in patterns
  char *jobcode;

  // Base folder of all import subfolders. Input.
  char *base_folder;

  // Pattern to build import subfolders for imports with copy,
  // child of base_folder. Input
  char *target_subfolder_pattern;

  // Pattern to build file names for imports with copy. Input
  char *target_file_pattern;

  // Computed base_folder/target_subfolder from expanding patterns and variables.
  // Output.
  char *target_dir;

  // Number of elements to import
  const int elements;

  // List of pathes of files that couldn't be imported due to filesystem errors or overrides.
  GList *discarded;

  /**
   * @brief Optional per-file completion method.
   *
   * The import worker calls this method after each source file has either been
   * imported successfully or rejected. The callback data belongs to this
   * structure and is released with callback_data_free after the complete job.
   */
  void (*file_imported)(const char *source, gboolean success, gpointer user_data);
  gpointer callback_data;
  GDestroyNotify callback_data_free;

} dt_control_import_t;


// free the internal strings of a dt_control_import_t structure. Doesn't free the structure itself.
void dt_control_import_data_free(dt_control_import_t *data);


/**
 * @brief Build a full path for a given image file, given a pattern.
 *
 * @param filename Full path of the original file
 * @param index Incremental number in a sequence
 * @param img dt_image_t object. Needs to be inited with EXIF fields prior to calling this function, otherwise EXIF variables are expanded to defaults/fallback.
 * @param data Import options
 * @return gchar* The full path after variables expansion
 */
gchar *dt_build_filename_from_pattern(const char *const filename, const int index, dt_image_t *img, dt_control_import_t *data);


/**
 * @brief Process a list of images to import with or without copying the files on an arbitrary hard-drive.
 *
 * @param data import informations to transmit through the functions
 * @return int 0 when the import job was queued, non-zero on failure
 */
int dt_control_import(dt_control_import_t data);


#ifdef __cplusplus
}
#endif
#endif // DT_CONTROL_JOBS_IMPORT_JOBS_H
