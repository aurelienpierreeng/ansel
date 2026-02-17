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
#pragma once

#include "common/image.h"
#include "control/control.h"
#include "common/variables.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct dt_control_import_t
{
  GList *imgs;
  GDateTime *datetime;
  gboolean copy;

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
 */
void dt_control_import(dt_control_import_t data);


#ifdef __cplusplus
}
#endif
