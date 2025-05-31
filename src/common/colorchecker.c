/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers,
    Copyright (C) 2025 Guillaume Stutin.

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

/**
 * ANSI CGATS.17 is THE standard text file format for exchanging color measurement data.
 * This standard text format (the ASCII version is by far the most common) is the format
 * accepted by most color measurement and profiling applications.
 * They can be used with lcms2.
 *
 * IT8 targets contain 288 patches in total.
 * At the bottom of the chart, there is a grey scale consisting of 22 patches (labeled GS01 to GS22),
 * flanked on each side by a Dmin and a Dmax patch, which are usually
 * labeled as Dmin or GS0, and Dmax or GS23.
 */

#include "colorchecker.h"
#include "common/colorspaces_inline_conversions.h"
#include "darktable.h"
#include "file_location.h"

#include <glib.h>
#include <inttypes.h>
#include <lcms2.h>

static inline dt_colorchecker_material_types _dt_colorchecker_IT8_get_material_type(const cmsHANDLE *hIT8)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return COLOR_CHECKER_MATERIAL_UNKNOWN;
  }

  const char *CGATS_type = cmsIT8GetSheetType(*hIT8);
  if(!CGATS_type) return COLOR_CHECKER_MATERIAL_UNKNOWN;

  if(g_strcmp0(CGATS_type, CGATS_types[CGATS_TYPE_IT8_7_1]) == 0)
    return COLOR_CHECKER_MATERIAL_TRANSPARENT;

  else if(g_strcmp0(CGATS_type, CGATS_types[CGATS_TYPE_IT8_7_2]) == 0)
    return COLOR_CHECKER_MATERIAL_OPAQUE;

  // if something went wrong
  return COLOR_CHECKER_MATERIAL_UNKNOWN; 
}


/**
 * @brief Gets the string representation of the material type ("Transparent" or "Opaque") to be used in label name.
 * The caller is responsible for freeing the returned string.
 * 
 * @param material (dt_colorchecker_material_types) the material type of the color checker
 * @return gchar* The string representation of the material type, or NULL if unknown.
 */
static inline const char *_dt_colorchecker_get_material_string(const dt_colorchecker_material_types material)
{
  if (material >= COLOR_CHECKER_MATERIAL_TRANSPARENT && material < COLOR_CHECKER_MATERIAL_UNKNOWN)
    return colorchecker_material_types[material];
  
  // else
  return NULL;
}

static inline dt_colorchecker_CGATS_types _dt_CGATS_get_type_value(const char *type)
{
  dt_colorchecker_CGATS_types t = CGATS_TYPE_IT8_7_1;

  // Ensure t doesn't overflows CGATS_types 
  while(type && t < CGATS_TYPE_UNKOWN)
  {
    if(!g_strcmp0(type, CGATS_types[t])) break;
    t++;
  }
  return t;
}

const dt_colorchecker_CGATS_spec_t _dt_colorchecker_get_CGATS_spec(const char *type)
{
  if(type)
  {
    const dt_colorchecker_CGATS_types t = _dt_CGATS_get_type_value(type);

    switch(t)
    {
      case CGATS_TYPE_IT8_7_1:
      case CGATS_TYPE_IT8_7_2:
        return IT8_7;
      case CGATS_TYPE_UNKOWN:
        fprintf(stderr, "Unknown CGATS type: %s\n", type);
        return IT8_7;
    }
  }

  return IT8_7;
}

/**
 * @brief Test if the file is a CGATS.17 file
 * and if it contains one table of patch only.
 *
 * @param data pointer to the cmsHANDLE
 * @return gboolean TRUE if the file is valid, FALSE otherwise.
 */
gboolean _dt_CGATS_is_supported(const cmsHANDLE *hIT8)
{
  gboolean valid = TRUE;

  if(!hIT8)
  {
    fprintf(stderr, "Error loading IT8 file.\n");
    valid = FALSE;
    goto end;
  }
  else
  {
    const char *CGATS_type = cmsIT8GetProperty(*hIT8, "CGATS");
    // Check if the data type can be found in our supported list of CGATS types
    if(_dt_CGATS_get_type_value(CGATS_type) == CGATS_TYPE_UNKOWN)
    {
      fprintf(stderr, "Warning: type '%s' is not supported by Ansel.\n", CGATS_type);
      valid = FALSE;
      goto end;
    }

    uint32_t table_count = cmsIT8TableCount(*hIT8);
    if(table_count != 1)
    {
      fprintf(stderr, "Warning: the CGATS.17 file contains %u table(s) but we only support files"
              "with one table at the moment.\n", table_count);
      valid = FALSE;
      goto end;
    }

    // Check if the CGATS file contains the expected number of patches
    const dt_colorchecker_CGATS_spec_t CGATS_spec = _dt_colorchecker_get_CGATS_spec(CGATS_type);
    const int num_patches = (const int)cmsIT8GetPropertyDbl(*hIT8, "NUMBER_OF_SETS");

    if(CGATS_spec.patches != num_patches)
    {
      fprintf(stderr, "Warning: the CGATS.17 file contains %d patches but we expect %d patches.\n",
              num_patches, CGATS_spec.patches);
      valid = FALSE;
      goto end;
    }
  }

end:
  return valid;
}

static inline const char *_dt_CGATS_get_author(const cmsHANDLE *hIT8)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return "Unknown Author";
  }
  const char *author = cmsIT8GetProperty(*hIT8, "ORIGINATOR");

  return author ? author : "Unknown Author";
}

/**
 * @brief Get the production date of the CGATS file.
 *
 * @param hIT8 the CGATS file handle
 * @return const char* a pointer to the date from the CGATS file
 */
static inline const char *_dt_CGATS_get_date(const cmsHANDLE *hIT8)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return "Unknown Date";
  }

  // in CGATS.17, the date in PROD_DATE is stored in the format YYYY:MM
  const char *date = cmsIT8GetProperty(*hIT8, "PROD_DATE");

  return date ? date : "Unknown Date";
}

/**
 * @brief Modify the date format from "YYYY:MM" given by a CGATS.17 file to the label form "Mon YYYY".
 * The caller is responsible for freeing the returned string.
 * 
 * @param date the date in the format "YYYY:MM"
 * @return gchar* String with the date in the format "Mon YYYY", or the original date otherwise.
 */
static inline gchar *_dt_CGATS_get_format_date(const cmsHANDLE *hIT8)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return g_strdup("Unknown Date");
  }

  const char *date = _dt_CGATS_get_date(hIT8);

  gchar *result = NULL;

  gchar **parts = g_strsplit_set(date, ":", 0);
  if(parts && parts[0] && parts[1])
  {
    const char *month[12]
        = { "Jan ", "Feb ", "Mar ", "Apr ", "May ", "Jun ", "Jul ", "Aug ", "Sep ", "Oct ", "Nov ", "Dec " };
    int month_num = atoi(parts[1]);
    if(month_num >= 1 && month_num <= 12)
      parts[1] = g_strdup(month[month_num - 1]);

    // write a new date in the format "Month(short) YYYY"
    gchar *date_fmt = g_strdup_printf("%s%s", parts[1], parts[0]);
    result = g_strdup(date_fmt);
    g_free(date_fmt);
  }
  else
    result = g_strdup(date);

  g_strfreev(parts);

  return result;
}

static inline const char *_dt_CGATS_get_manufacturer(const cmsHANDLE *hIT8)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return "Unknown Manufacturer";
  }
  const char *manufacturer = cmsIT8GetProperty(*hIT8, "MANUFACTURER");
  return manufacturer ? manufacturer : "Unknown Manufacturer";
}

/**
 * @brief Get the name of a built-in color checker.
 *
 * @param target_type The type of colorchecker
 * @return char* The name of the colorchecker
 */
static inline const char *dt_get_builtin_colorchecker_name(const dt_color_checker_targets target_type)
{
  const dt_color_checker_t *color_checker = dt_get_color_checker(target_type, NULL);
  const char *name = color_checker->name;
  dt_print(DT_DEBUG_VERBOSE, _("dt_get_builtin_colorchecker_name: %s\n"), name);
  return name;
}

/**
 * @brief build a name for the colorchecker.
 * The returned string must be freed by the caller.
 *
 * @param label the struct containing useful data to build a label
 * @return gchar* String with the name of the colorchecker
 */
static inline gchar *_dt_colorchecker_label_build_name(const dt_colorchecker_CGATS_label_name_t label)
{
  gchar *name = NULL;
  
  // Build the name with the format: type (material) - date
  gchar *tmp_originator = NULL;
  gchar *tmp_date = NULL;
  gchar *tmp_material = NULL;
  // author if any
  if(label.originator && g_strcmp0(label.originator, "")) tmp_originator = g_strdup_printf(" - %s", label.originator);
  // date if any
  if(label.date && g_strcmp0(label.date, "")) tmp_date = g_strdup_printf(" %s", label.date);
  // material if any
  if(label.material && g_strcmp0(label.material, "")) tmp_material = g_strdup_printf(" (%s)", label.material);
  
  // Compose: filename
  name = g_strdup_printf("%s%s%s%s", label.type, tmp_material, tmp_date, tmp_originator);

  if(tmp_originator) g_free(tmp_originator);
  if(tmp_date) g_free(tmp_date);

  return name;
}

/**
 * @brief Get the name of the colorchecker from the CGATS file.
 * The resulting string must be freed by the caller.
 *
 * @param hIT8 the CGATS file handle
 * @param filename the CGATS file name, used if the CGATS file does not contain a name.
 * @return char* String with the name of the colorchecker
 */
static inline char *_dt_CGATS_get_name(const cmsHANDLE *hIT8, const char *filename)
{
  gchar *result = NULL;

  if(!hIT8)
  {
    fprintf(stderr, "dt_CGATS_get_name: Error: Invalid CGATS handle provided.\n");
    return g_strdup("Unnamed CGATS");
  }

  gchar *basename = NULL;
  if(filename && g_strcmp0(filename, ""))
  {
    basename = g_path_get_basename(filename);
    char *dot = g_strrstr(basename, ".");
    if(dot)
    {
      // remove the file extension
      *dot = '\0';
    }
  }

  // Get other useful information from the CGATS file
  const char *CGATS_type = cmsIT8GetSheetType(*hIT8);
  const dt_colorchecker_CGATS_spec_t CGATS_spec = _dt_colorchecker_get_CGATS_spec(CGATS_type);
  
  const char *originator = _dt_CGATS_get_author(hIT8);
  const dt_colorchecker_material_types material = _dt_colorchecker_IT8_get_material_type(hIT8);
  gchar *material_str = g_strdup(_dt_colorchecker_get_material_string(material));
  gchar *date = _dt_CGATS_get_format_date(hIT8);

  dt_colorchecker_CGATS_label_name_t label = { .type = CGATS_spec.type,
                                               .originator = originator,
                                               .date = date,
                                               .material = material_str }; //can be NULL

  gchar *name = _dt_colorchecker_label_build_name(label);
  
  // clean up
  g_free(date);
  if(material_str) g_free(material_str);

  if(name)
    result = name;
  else
    result = g_strdup(basename && g_strcmp0(basename, "") ? basename : "Unnamed CGATS");

  if(basename) g_free(basename);

  return result;
}

static float dE_1976(const float a, const float b, const float c)
{
  return sqrtf(sqf(a) + sqf(b) + sqf(c));
}

static inline void _dt_CGATS_find_whitest_blackest_greyest(const dt_color_checker_patch *const values, size_t *bwg, const size_t patch)
{
  for(int i = 0; i < 3; i++)
  {
    float target = 50.f * i;
    float delta_current = dE_1976(values[bwg[i]].Lab[0] - target, values[bwg[i]].Lab[1], values[bwg[i]].Lab[2]);
    float delta_patch = dE_1976(values[patch].Lab[0] - target, values[patch].Lab[1], values[patch].Lab[2]);
    if(delta_patch < delta_current)
      bwg[i] = patch;
  }
}

/**
 * @brief fills the patch values from the CGATS file, converts to Lab if needed.
 * The number of patches to be filled is given by the CGATS file.
 *
 * @param hIT8 the CGATS file handle
 * @param values the array of patches to be filled
 * @param bwg the float* [3] that will get the blackest, whitest and greyest patches to be found
 * @return NULL if error, dt_color_checker_patch* with the colors otherwise.
 */
int _dt_colorchecker_CGATS_fill_patch_values(cmsHANDLE hIT8, dt_color_checker_patch *values, size_t *bwg)
{
  int error = 0;
  if(!values) error = 1;

  int column_SAMPLE_ID = -1;
  int column_X = -1;
  int column_Y = -1;
  int column_Z = -1;
  int column_L = -1;
  int column_a = -1;
  int column_b = -1;
  char **sample_names = NULL;
  int n_columns = cmsIT8EnumDataFormat(hIT8, &sample_names);
  size_t num_patches = (size_t)cmsIT8GetPropertyDbl(hIT8, "NUMBER_OF_SETS");
  const char *CGATS_type = cmsIT8GetSheetType(hIT8);
  const dt_colorchecker_CGATS_spec_t CGATS_spec = _dt_colorchecker_get_CGATS_spec(CGATS_type);

  gboolean use_XYZ = FALSE;
  if(n_columns == -1)
  {
    fprintf(stderr, "error with the CGATS file, can't get column types\n");
    error = 1;
  }

  for(int i = 0; i < n_columns; i++)
  {
    if(!g_strcmp0(sample_names[i], "SAMPLE_ID"))
      column_SAMPLE_ID = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_X"))
      column_X = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_Y"))
      column_Y = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_Z"))
      column_Z = i;
    else if(!g_strcmp0(sample_names[i], "LAB_L"))
      column_L = i;
    else if(!g_strcmp0(sample_names[i], "LAB_A"))
      column_a = i;
    else if(!g_strcmp0(sample_names[i], "LAB_B"))
      column_b = i;
  }

  if(column_SAMPLE_ID == -1)
  {
    fprintf(stderr, "error: can't find the SAMPLE_ID column in the CGATS file.\n");
    error = 1;
  }

  int columns[3] = { -1, -1, -1 };
  if(column_L != -1 && column_a != -1 && column_b != -1)
  {
    columns[0] = cmsIT8FindDataFormat(hIT8, "LAB_L");
    columns[1] = cmsIT8FindDataFormat(hIT8, "LAB_A");
    columns[2] = cmsIT8FindDataFormat(hIT8, "LAB_B");
  }
  // In case no Lab column is found, we assume the IT8 file has XYZ data
  else if(column_X != -1 && column_Y != -1 && column_Z != -1)
  {
    use_XYZ = TRUE;
    columns[0] = cmsIT8FindDataFormat(hIT8, "XYZ_X");
    columns[1] = cmsIT8FindDataFormat(hIT8, "XYZ_Y");
    columns[2] = cmsIT8FindDataFormat(hIT8, "XYZ_Z");
  }
  else
  {
    fprintf(stderr, "error: can't find XYZ or Lab columns in the CGATS file\n");
    error = 1;
  }

  // Chart dimensions
  const int cols = CGATS_spec.colums;
  const int rows = CGATS_spec.rows;
  // Patch size in ratio of the chart size
  const float patch_size_x = CGATS_spec.patch_width;
  const float patch_size_y = CGATS_spec.patch_height;

  // Offset ratio of the center of the patch from the border of the chart
  const float patch_offset_x = CGATS_spec.patch_offset_x;
  const float patch_offset_y = CGATS_spec.patch_offset_y;

  #ifdef OPENMP
  #pragma omp parallel for
  #endif
  for(size_t patch_iter = 0; patch_iter < num_patches; patch_iter++)
  {
    values[patch_iter].name = g_strdup(cmsIT8GetDataRowCol(hIT8, patch_iter, 0));
    if(values[patch_iter].name == NULL)
    {
      #ifdef OPENMP
      #pragma omp critical
      {
      #endif
        fprintf(stderr, "error : can't find sample '%lu' in CGATS file\n", patch_iter);
        error = 1;
      #ifdef OPENMP
      }
      #endif
      continue;
    }

    // IT8 grey scale patches
    if(!g_strcmp0(CGATS_spec.type, "IT8") && patch_iter + 1 > cols * rows)
    {
      int grey_patches_iter = ((int)patch_iter + 1) - cols * rows;
      // calculate the grey patch's horizontal and vertical position in the chart
      values[patch_iter].x = ((float)grey_patches_iter - 0.75f) * patch_size_x;
      values[patch_iter].y = 14.5f * patch_size_y;
    }
    else
    {
      // find the patch's horizontal position in the chart
      values[patch_iter].x = (float)(patch_iter % cols) * patch_size_x;
      values[patch_iter].x += patch_offset_x;

      // find the patch's vertical position in the chart
      values[patch_iter].y = (float)((int)(patch_iter / cols)); // the result must be an integer
      values[patch_iter].y *= patch_size_y;
      values[patch_iter].y += patch_offset_y;
    }

    const double patchdbl[3] = { cmsIT8GetDataRowColDbl(hIT8, (int)patch_iter, columns[0]),
                                 cmsIT8GetDataRowColDbl(hIT8, (int)patch_iter, columns[1]),
                                 cmsIT8GetDataRowColDbl(hIT8, (int)patch_iter, columns[2]) };

    const dt_aligned_pixel_t patch_color = { (float)patchdbl[0], (float)patchdbl[1], (float)patchdbl[2], 0.0f };

    // Convert to Lab when it's in XYZ
    if(use_XYZ)
      dt_XYZ_to_Lab(patch_color, values[patch_iter].Lab);
    else
    {
      values[patch_iter].Lab[0] = patch_color[0];
      values[patch_iter].Lab[1] = patch_color[1];
      values[patch_iter].Lab[2] = patch_color[2];
    }

    #ifdef OPENMP
    #pragma omp critical
    {
    #endif
      _dt_CGATS_find_whitest_blackest_greyest(values, bwg, patch_iter);
    #ifdef OPENMP
    }
    #endif
  }

  return error;
}

dt_color_checker_t *dt_colorchecker_user_ref_create(const char *filename)
{
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    fprintf(stderr, "Error: the file '%s' does not exist or is not a regular file.\n", filename);
    return NULL;
  }
  
  cmsHANDLE hIT8 = cmsIT8LoadFromFile(NULL, filename);

  if(!_dt_CGATS_is_supported(&hIT8))
  {
    fprintf(stderr, "Ansel cannot load the CGATS file '%s'\n", filename);
    if(hIT8) cmsIT8Free(hIT8);
    return NULL;

  }

  const char *type = cmsIT8GetSheetType(hIT8);
  const dt_colorchecker_CGATS_spec_t CGATS_spec = _dt_colorchecker_get_CGATS_spec(type);
  size_t num_patches = (size_t)cmsIT8GetPropertyDbl(hIT8, "NUMBER_OF_SETS");
  size_t total_size = sizeof(dt_color_checker_t) + num_patches * sizeof(dt_color_checker_patch);

  dt_color_checker_t *checker = dt_colorchecker_init(total_size);

  if(!checker)
  {
    fprintf(stderr, "Error: can't allocate memory for the color checker from IT8 chart\n");
    if(hIT8) cmsIT8Free(hIT8);
    free(checker);
    return NULL;
  }

  checker->name = _dt_CGATS_get_name(&hIT8, filename);
  checker->author = g_strdup(_dt_CGATS_get_author(&hIT8));
  checker->date = g_strdup(_dt_CGATS_get_date(&hIT8));
  checker->manufacturer = g_strdup(_dt_CGATS_get_manufacturer(&hIT8));
  checker->type = COLOR_CHECKER_USER_REF;
  checker->radius = CGATS_spec.radius;
  checker->ratio = CGATS_spec.ratio;
  checker->patches = num_patches;
  checker->size[0] = CGATS_spec.size[0];
  checker->size[1] = CGATS_spec.size[1];
  checker->middle_grey = CGATS_spec.middle_grey;
  checker->white = CGATS_spec.white;
  checker->black = CGATS_spec.black;

  // blackest, whitest and greyest patches will be found while filling the color values
  size_t bwg[3] = { 0, 0, 0 };
  _dt_colorchecker_CGATS_fill_patch_values(hIT8, checker->values, bwg);

  checker->black = bwg[0];
  checker->white = bwg[1];
  checker->middle_grey = bwg[2];
  dt_print(DT_DEBUG_VERBOSE, _("blackest patch: %s, middle grey patch: %s, white patch: %s\n"),
           checker->values[bwg[0]].name, checker->values[bwg[1]].name, checker->values[bwg[2]].name);

  dt_print(DT_DEBUG_VERBOSE, _("it8 '%s' done\n"), filename);

  if(hIT8) cmsIT8Free(hIT8);
  return checker;
}

static dt_colorchecker_label_t *_dt_colorchecker_user_ref_add_label(const gchar *filename, const gchar *user_it8_dir)
{
  dt_colorchecker_label_t *result = NULL;

  gchar *filepath = g_build_filename(user_it8_dir, filename, NULL);
  if(g_file_test(filepath, G_FILE_TEST_IS_REGULAR))
  {
    cmsHANDLE hIT8 = cmsIT8LoadFromFile(NULL, filepath);

    if(hIT8 && _dt_CGATS_is_supported(&hIT8))
    {
      gchar *label = _dt_CGATS_get_name(&hIT8, filename);
      dt_colorchecker_label_t *CGATS_label = dt_colorchecker_label_init(label, COLOR_CHECKER_USER_REF, filepath);
          
      g_free(label);
      result = CGATS_label;
      if(!result) goto error;
    }
    cmsIT8Free(hIT8);
  }
  g_free(filepath);

  return result;

error:
  free(result);
  return NULL;
}

int dt_colorchecker_find_builtin(GList **colorcheckers_label)
{
  int nb = 0;
  for(int k = 0; k < COLOR_CHECKER_USER_REF; k++)
  {
    const char *name = dt_get_builtin_colorchecker_name(k);
    dt_colorchecker_label_t *builtin_label = dt_colorchecker_label_init(name, k, NULL);

    if(!builtin_label)
    {
      fprintf(stderr, "dt_colorchecker_find: failed to allocate memory for builtin colorchecker label %d\n", k);
      continue;
    }
    else
    {
      *colorcheckers_label = g_list_append(*colorcheckers_label, builtin_label);
      nb++;
    }
  }
  return nb;
}

int dt_colorchecker_find_CGAT_reference_files(GList **ref_colorcheckers_files)
{
  int nb = 0;
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  gchar *user_it8_dir = g_build_filename(confdir, "color", "it8", NULL);

  GDir *dir = g_dir_open(user_it8_dir, 0, NULL);
  if(dir)
  {
    const char *filename;
    while((filename = g_dir_read_name(dir)) != NULL)
    {
      dt_colorchecker_label_t *CGATS_label = _dt_colorchecker_user_ref_add_label(filename, user_it8_dir);
      if(CGATS_label)
      {
        *ref_colorcheckers_files = g_list_append(*ref_colorcheckers_files, CGATS_label);
        nb++;
      }
      else
        fprintf(stderr, "Error: failed to load CGATS file '%s' in %s\n", filename, user_it8_dir);
      
    }
    g_dir_close(dir);
  }
  g_free(user_it8_dir);

  return nb;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
