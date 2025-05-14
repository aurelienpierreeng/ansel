/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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

#include "colorchecker_it8.h"
#include "common/colorchecker.h"
#include "common/colorspaces.h"
//#include "control/control.h"
#include "darktable.h"
#include "colorchecker_it8.h"

#include <glib.h>
#include <inttypes.h>
#include <lcms2.h>

gboolean dt_colorchecker_it8_valid(void * data)
{
  gboolean valid = TRUE;
  cmsHANDLE hIT8 = *(cmsHANDLE *)data;
  
  if(!hIT8)
  {
    fprintf(stderr, "Error loading IT8 file.\n");
    valid = FALSE;
  }
  else
  {
    uint32_t table_count = cmsIT8TableCount(hIT8);
    if( table_count != 1)
    {
      fprintf(stderr, "Error with the IT8 file, it contains %u table(s) but we only support files with one table at the moment.\n", table_count);
      valid = FALSE;
    }
  }
  return valid;
}

static inline char *_dt_colorchecker_it8_get_name(const cmsHANDLE *hIT8)
{
  if(!hIT8) 
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return g_strdup("Unnamed IT8");
  }
  const char *name = cmsIT8GetProperty(*hIT8, "NAME");
  return g_strdup(name ? name : "Unnamed IT8");
}

static inline char *_dt_colorchecker_it8_get_author(const cmsHANDLE *hIT8)
{
  if(!hIT8) 
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return g_strdup("Unknown Author");
  }
  const char *author = cmsIT8GetProperty(*hIT8, "AUTHOR");
  return g_strdup(author ? author : "Unknown Author");
}

static inline char *_dt_colorchecker_it8_get_date(const cmsHANDLE *hIT8)
{
  if(!hIT8) 
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return g_strdup("Unknown Date");
  }
  const char *date = cmsIT8GetProperty(*hIT8, "DATE");
  return g_strdup(date ? date : "Unknown Date");
}

static inline char *_dt_colorchecker_it8_get_manufacturer(const cmsHANDLE *hIT8)
{
  if(!hIT8) 
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return g_strdup("Unknown Manufacturer");
  }
  const char *manufacturer = cmsIT8GetProperty(*hIT8, "MANUFACTURER");
  return g_strdup(manufacturer ? manufacturer : "Unknown Manufacturer");
}

/**
 * @brief fills the patch values from the IT8 file, converts to Lab if needed.
 * 
 * @param hIT8 
 * @param num_patches 
 * @return NULL if error, dt_color_checker_patch* with the colors otherwise.
 */
int _dt_colorchecker_it8_fill_patch_values(cmsHANDLE hIT8, dt_color_checker_patch *values, size_t count)
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
  gboolean use_XYZ = FALSE;
  if(n_columns == -1)
  {
    fprintf(stderr, "error with the IT8 file, can't get column types\n");
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
    fprintf(stderr, "error with the IT8 file, can't find the SAMPLE_ID column\n");
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
    fprintf(stderr, "error with the IT8 file, can't find XYZ or Lab columns\n");
    error = 1;
  }

  // IT8 chart dimensions
  const int cols = 22;
  const int rows = 12;  
  // Patch size in ratio of the chart size
  const float patch_size_x = 1.0f / (cols + 1);
  const float patch_size_y = 1.0f / (rows + 1);

  // Offset to the center of the patch and one patch-size equivalent from the border
  const float patch_offset_x = patch_size_x;
  const float patch_offset_y = patch_size_y;

  for(int patch_iter = 0; patch_iter < count; patch_iter++)
  {
    values[patch_iter].name = cmsIT8GetDataRowCol(hIT8, patch_iter, 0); 
    if( values[patch_iter].name == NULL)
    {
      {
        fprintf(stderr, "error with the IT8 file, can't find sample '%d'\n", patch_iter);
        error = 1;
      }
      continue;
    }

    if(patch_iter + 1 <= cols * rows) // Color patches
    {
      // find the patch's horizontal position in the guide
      values[patch_iter].x = (patch_iter % cols) * patch_size_x;
      values[patch_iter].x += patch_offset_x;

      // find the patch's vertical position in the guide
      values[patch_iter].y = (int)patch_iter / cols; // The result must be an int
      values[patch_iter].y *= patch_size_y;
      values[patch_iter].y += patch_offset_y;
    }
    else // Grey botom strip patch
    {
      int grey_patch = (patch_iter + 1) - cols * rows;
      // find the patch's horizontal position in the guide
      values[patch_iter].x = -patch_size_x + patch_size_x * grey_patch;

      // find the patch's vertical position in the guide
      values[patch_iter].y = 14 * patch_size_y;
    }


    const double patchdbl[3] = {
      cmsIT8GetDataRowColDbl(hIT8, patch_iter, columns[0]),
      cmsIT8GetDataRowColDbl(hIT8, patch_iter, columns[1]),
      cmsIT8GetDataRowColDbl(hIT8, patch_iter, columns[2]) };

    const dt_aligned_pixel_t patch_color = {
      (float)patchdbl[0],
      (float)patchdbl[1],
      (float)patchdbl[2],
      0.0f }; 
    
    // Convert to Lab when it's in XYZ
    if(use_XYZ)
      dt_XYZ_to_Lab(patch_color, values[patch_iter].Lab);
    else
    {
      values[patch_iter].Lab[0] = patch_color[0];
      values[patch_iter].Lab[1] = patch_color[1];
      values[patch_iter].Lab[2] = patch_color[2];
    }
  }

  return error;
}

dt_color_checker_t *dt_colorchecker_it8_create(const char *filename)
{
  int error = 0;
  cmsHANDLE hIT8 = cmsIT8LoadFromFile(NULL, filename);

  if(!dt_colorchecker_it8_valid(&hIT8))
  {
    fprintf(stderr, "Ansel cannot load IT8 file '%s'\n", filename);
    //dt_control_log(_("Ansel cannot load IT8 file '%s'"), filename);

    error = 1;
    goto end;
  }

  size_t num_patches = (size_t)cmsIT8GetPropertyDbl(hIT8, "NUMBER_OF_SETS");
  size_t total_size = sizeof(dt_color_checker_t) + num_patches * sizeof(dt_color_checker_patch);

  dt_color_checker_t *checker = malloc(total_size);
  if (!checker)
  {
    fprintf(stderr, "Error: can't allocate memory for the color checker from IT8 chart\n");
    error = 1;
    goto end;
  }

  *checker = (dt_color_checker_t){
            .name = _dt_colorchecker_it8_get_name(&hIT8),
            .author = _dt_colorchecker_it8_get_author(&hIT8),
            .date = _dt_colorchecker_it8_get_date(&hIT8),
            .manufacturer = _dt_colorchecker_it8_get_manufacturer(&hIT8),
            .type = COLOR_CHECKER_IT8,
            .radius = 0.0379f, 
            .ratio = 13.f / 23.f,
            .patches = num_patches,
            .size = {23, 13},
            .middle_grey = 273, // 10th patch on the bottom grey strip
            .white = 263, // 1st patch on the bottom grey strip
            .black = 287 }; // last patch on the bottom grey strip

  _dt_colorchecker_it8_fill_patch_values(hIT8, checker->values, num_patches);

  fprintf(stdout, "it8 '%s' done\n", filename);

  /*if(!checker.values)
  {
    fprintf(stderr, "Error: can't allocate memory for the color checker from IT8 chart\n");
    error = 1;
  }*/

end:
  if(hIT8) cmsIT8Free(hIT8);
  return error ? NULL : checker;
}

/*
void dt_colorchecker_it8_cleanup(dt_color_checker_t *checker)
{ 
  if(checker)
  {
    free(checker->values);
    free(checker);
  }
}
*/
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
