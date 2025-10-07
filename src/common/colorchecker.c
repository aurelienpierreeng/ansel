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

// In some environments ERROR is already defined, ie: WIN32
#if defined(ERROR)
#undef ERROR
#endif // defined (ERROR)

#define ERROR           \
  {                     \
    lineno = __LINE__;  \
    goto error;         \
  }

typedef enum parser_state_t {
  BLOCK_NONE = 0,
  BLOCK_BOXES,
  BLOCK_BOX_SHRINK,
  BLOCK_REF_ROTATION,
  BLOCK_XLIST,
  BLOCK_YLIST,
  BLOCK_EXPECTED
} parser_state_t;

typedef struct cht_box_t {
  char key_letter; // 'D', 'X', or 'Y'
  char *label_x_start;
  char *label_x_end;
  char *label_y_start;
  char *label_y_end;
  float width;
  float height;
  float x_origin;
  float y_origin;
  float x_increment;
  float y_increment;
} cht_box_t;

typedef struct cht_box_F_t {
  float ax; // top left corner
  float ay; // top left corner
  float bx; // top right corner
  float by; // top right corner
  float cx; // bottom left corner
  float cy; // bottom left corner
  float dx; // bottom right corner
  float dy; // bottom right corner
  float width; // width of the frame
  float height; // height of the frame
} cht_box_F_t;

#define TWO_SQRT2f 2.8284271247461900976f // sqrt(2) * 2

static void _dt_colorchecker_copy_patch(dt_color_checker_patch *dest, const dt_color_checker_patch *src)
{
  if(!dest || !src) return;

  dest->name = g_strdup(src->name);
  dest->x = src->x;
  dest->y = src->y;
  dest->Lab[0] = src->Lab[0];
  dest->Lab[1] = src->Lab[1];
  dest->Lab[2] = src->Lab[2];
}

void dt_colorchecker_copy(dt_color_checker_t *dest, const dt_color_checker_t *src)
{
  if(!dest || !src) return;

  dest->name = g_strdup(src->name);
  dest->author = g_strdup(src->author);
  dest->date = g_strdup(src->date);
  dest->manufacturer = g_strdup(src->manufacturer);
  dest->type = src->type;
  dest->radius = src->radius;
  dest->ratio = src->ratio;
  dest->patches = src->patches;
  dest->size[0] = src->size[0];
  dest->size[1] = src->size[1];
  dest->middle_grey = src->middle_grey;
  dest->white = src->white;
  dest->black = src->black;

  if(src->values)
  {
    dest->values = dt_colorchecker_patch_array_init(src->patches);
    if(!dest->values)
    {
      fprintf(stderr, "Error: Memory allocation failed for color checker values.\n");
      return;
    }
    
    for(int i = 0; i < src->patches; i++)
    {
      _dt_colorchecker_copy_patch(&dest->values[i], &src->values[i]);
    }
  }
  else
  {
    dest->values = NULL;
  }
  dest->finished = TRUE;
}

/**
 * @brief Extracts the frame coordinates from the tokens, computes the width and height.
 * 
 * @param tokens An array of strings containing the data from a F box in a .cht file.
 * @return cht_box_F_t* A pointer to a cht_box_F_t structure containing the extracted coordinates and dimensions.
 */
static cht_box_F_t *_dt_cht_extract_F(const char **tokens)
{
  cht_box_F_t *frame_coordinates = (cht_box_F_t *)malloc(sizeof(cht_box_F_t));
  if(!frame_coordinates) return NULL;

  size_t index = 0;
  float extracted_coords[8] = { 0.f };
  for(size_t i = 0; tokens[i] != NULL; i++)
  {
    if(index >= 8) break; // Prevent overflow

    if(g_ascii_isdigit(tokens[i][0])) // note : always positive numbers
    {
      extracted_coords[index] = (float)g_ascii_strtod(tokens[i], NULL);
      index++;
    }
  }

  // copy the extracted coordinates to the frame_coordinates structure
  frame_coordinates->ax = extracted_coords[0];
  frame_coordinates->ay = extracted_coords[1];
  frame_coordinates->bx = extracted_coords[2];
  frame_coordinates->by = extracted_coords[3];
  frame_coordinates->cx = extracted_coords[4];
  frame_coordinates->cy = extracted_coords[5];
  frame_coordinates->dx = extracted_coords[6];
  frame_coordinates->dy = extracted_coords[7];

  // Compute the width and height of the frame
  frame_coordinates->width = extracted_coords[2] - extracted_coords[0];
  frame_coordinates->height = extracted_coords[5] - extracted_coords[1];
  
  return frame_coordinates;
}

static dt_colorchecker_chart_spec_t *_dt_colorchecker_chart_spec_init()
{
  dt_colorchecker_chart_spec_t *result = (dt_colorchecker_chart_spec_t *)malloc(sizeof(dt_colorchecker_chart_spec_t));
  if(!result) return NULL;

  result->type = NULL;
  result->radius = 0.f;
  result->ratio = 0.f;
  result->size[0] = 0;
  result->size[1] = 0;
  result->middle_grey = 0;
  result->white = 0;
  result->black = 0;
  result->num_patches = 0;
  result->colums = 0;
  result->rows = 0;
  result->patch_width = FLT_MAX;
  result->patch_height = FLT_MAX;
  result->patch_offset_x = 0.f;
  result->patch_offset_y = 0.f;
  result->guide_size[0] = 0.f;
  result->guide_size[1] = 0.f;
  result->patches = NULL;

  return result;
}

static void _dt_colorchecker_chart_spec_cleanup(dt_colorchecker_chart_spec_t *chart_spec)
{
  if(!chart_spec) return;

  // Free the patches' gslist
  if(chart_spec->patches)
    g_slist_free_full(chart_spec->patches, dt_colorchecker_patch_cleanup_list);

  free(chart_spec);
  chart_spec = NULL;
}

static dt_color_checker_patch *_dt_colorchecker_patch_init()
{
  dt_color_checker_patch *patch = (dt_color_checker_patch *)malloc(sizeof(dt_color_checker_patch));
  if(!patch) return NULL;

  patch->name = NULL;
  patch->Lab[0] = 0.f;
  patch->Lab[1] = 0.f;
  patch->Lab[2] = 0.f;
  patch->x = -1.f;
  patch->y = -1.f;

  return patch;
}

static void _dt_cht_box_cleanup(void *data)
{
  cht_box_t *box = (cht_box_t *)data;
  if(!box) return;

  SAFE_FREE(box->label_x_start);
  SAFE_FREE(box->label_x_end);
  SAFE_FREE(box->label_y_start);
  SAFE_FREE(box->label_y_end);
  SAFE_FREE(box);
}

static cht_box_t *_dt_cht_box_extract(const char **tokens)
{
  cht_box_t *box = (cht_box_t *)calloc(1, sizeof(cht_box_t));
  if(!box) return NULL;

  size_t index = 0;
  size_t i = 0;
  while(tokens[i] != NULL && index <= 10)
  {
    if(tokens[i][0] != '\0')
    {
      float value = 0;
      const char *string = tokens[i];

      // Check if the token is a digit or a negative number before converting the string to float
      if(g_ascii_isdigit(tokens[i][0]) || (tokens[i][0] == '-' && g_ascii_isdigit(tokens[i][1])))
        value = (float)g_ascii_strtod(tokens[i], NULL);
      
      switch(index)
      {
        case 0: box->key_letter    = tokens[i][0]; index++; break; // 'D', 'X', or 'Y'
        case 1: box->label_x_start = g_strdup(string); index++; break;
        case 2: box->label_x_end   = g_strdup(string); index++; break;
        case 3: box->label_y_start = g_strdup(string); index++; break;
        case 4: box->label_y_end   = g_strdup(string); index++; break;      
        case 5: box->width         = value; index++; break;
        case 6: box->height        = value; index++; break;
        case 7: box->x_origin      = value; index++; break;
        case 8: box->y_origin      = value; index++; break;
        case 9: box->x_increment   = value; index++; break;
        case 10: box->y_increment  = value; index++; break;
        default: fprintf(stderr, "Unexpected token in cht box extraction: %s\n", tokens[i]);
                 _dt_cht_box_cleanup(box);
                 return NULL;
      }
    }
    i++;
  }

  return box;
}

/**
 * @brief Increments a string alphanumerically.
 * 
 * @param in The input string to increment.
 * @return char* A new string with the last character incremented. The caller is responsible for freeing the returned string.
 */
static char *_increment_string(const gchar *in)
{
  if (!in || *in == '\0') return NULL;

  gchar *result = g_strdup(in);
  size_t len = safe_strlen(result);

  if(len == 0) return NULL;

  for(int i = (int)len - 1; i >= 0; i--)
  {
    // for numbers
    if (g_ascii_isdigit(result[i]))
    {
      if (result[i] == '9')
      {
        result[i] = '0';
        continue;
      }
      result[i]++;
      break;
    }
    // for letters
    else if (g_ascii_isalpha(result[i]))
    {
      if (result[i] == 'z' || result[i] == 'Z')
      {
        result[i] = (result[i] == 'z') ? 'a' : 'A';
        continue;
      }
      result[i]++;
      break;
    }
    // there should not be other cases
    else
    {
      break;
    }
  }

  return result;
}

/**
 * @brief Removes leading zeros from a string. 
 * 
 * @param in The input string.
 * @return char* A pointer to the first char following the leading zero.
 */
static inline const char *_remove_leading_zeros(const char *in)
{
  if(!in || *in == '\0') return "";
  const char *start = in;
  while(*start == '0') start++;

  return start;
}

/**
 * @brief Generates a list of patches from the provided cht_patch structure.
 * Patche's positions are calculated by iterating over the labels alphanumerically.
 * 
 * @param chart The structure to populate with patches.
 * @param cht_patch The structure containing the patch information.
 * @param F_box The cht_box_F_t structure containing the frame values.
 * @return gboolean Returns TRUE if the operation was successful, FALSE otherwise.
 */
static gboolean _dt_cht_generate_patch_list(dt_colorchecker_chart_spec_t *chart, const cht_box_t *cht_patch, const cht_box_F_t *F_box)
{
  gboolean result = FALSE;
  int lineno = 0;

  gchar *current_colum = NULL;
  gchar *current_row = NULL;
  gchar *last_label = NULL;

  // Input validation
  if(!cht_patch)
  {
    fprintf(stderr, "Invalid cht_patch");
    ERROR;
  }

  if(!chart)
  {
    fprintf(stderr, "Invalid chart");
    ERROR;
  }

  // The key letter determines the axes to begin to iterate
  gboolean swap_axes = (cht_patch->key_letter == 'Y') ? TRUE : FALSE;

  // Unpack strings from cht_patch
  const char *start_colum = swap_axes ? cht_patch->label_y_start : cht_patch->label_x_start;
  const char *end_colum = swap_axes ? cht_patch->label_y_end : cht_patch->label_x_end;

  const char *start_row = swap_axes ? cht_patch->label_x_start : cht_patch->label_y_start;
  const char *end_row = swap_axes ? cht_patch->label_x_end : cht_patch->label_y_end;

  // start shouldn't be greater than end
  if(g_strcmp0(start_colum, end_colum) > 0 || g_strcmp0(start_row, end_row) > 0)
    ERROR

  // we want the center of the patch.
  const float patch_w = cht_patch->width / 2;
  const float patch_h = cht_patch->height / 2;

  // Prepare the initial x and y coordinates
  float origin_x = cht_patch->x_origin - (chart->guide_size[0] / 2) + patch_w - F_box->ax;
  float origin_y = cht_patch->y_origin - (chart->guide_size[1] / 2) + patch_h - F_box->ay;

  // build the last label name, for comparison
  const char *last_label_colum = (end_colum[0] != '_') ? _remove_leading_zeros(end_colum) : NULL;
  const char *last_label_row = (end_row[0] != '_') ? _remove_leading_zeros(end_row) : NULL;
  last_label = g_strconcat(last_label_colum ? last_label_colum : "", last_label_row ? last_label_row : "", NULL);

  // Copy string for manipulation
  current_colum = g_strdup(start_colum);
  const char *colum_last = swap_axes ? cht_patch->label_y_end : cht_patch->label_x_end; 
  const char *row_last = swap_axes ? cht_patch->label_x_end : cht_patch->label_y_end;
  if(!current_colum) ERROR

  // iterate over the columns and rows
  int index_colum = 0;
  while(g_strcmp0(current_colum, colum_last) <= 0)
  {
    current_row = g_strdup(start_row);
    if(!current_row) ERROR
    int index_row = 0;

    while(g_strcmp0(current_row, row_last) <= 0)
    {
      // Create the label
      const char *label_colum = current_colum[0] != '_' ? _remove_leading_zeros(current_colum) : NULL;
      const char *label_row = current_row[0] != '_' ? _remove_leading_zeros(current_row) : NULL;

      const gchar *label = g_strconcat(label_colum ? label_colum : "", label_row ? label_row : "", NULL);
      if(!label) ERROR

      // Create the patch
      dt_color_checker_patch *patch = _dt_colorchecker_patch_init();
      if(!patch) ERROR

      // Set the patch properties
      patch->name = g_strdup(label);
      if(!patch->name) ERROR

      int index_x = swap_axes ? index_row : index_colum;
      float temp_x = origin_x + (cht_patch->x_increment * index_x);
      temp_x /= F_box->width - chart->guide_size[0]; // factorize to the frame width
      
      int index_y = swap_axes ? index_colum : index_row;
      float temp_y = origin_y + (cht_patch->y_increment * index_y);
      temp_y /= F_box->height - chart->guide_size[1]; // factorize to the frame height
      
      patch->x = temp_x;
      patch->y = temp_y;

      // Add to the list
      chart->patches = g_slist_append(chart->patches, patch);

      if(!g_strcmp0(label, last_label)) goto out;
      if(!g_strcmp0(current_row, "_")) break;

      // increment x in a new string and pass the ownership to current_row
      gchar *temp = _increment_string(current_row);
      SAFE_G_FREE(current_row)
      current_row = temp;
      
      chart->colums = MAX(chart->colums, index_row + 1);
      index_row++;
    }

    // increment y in a new string and pass the ownership to current_colum
    gchar *temp = _increment_string(current_colum);
    SAFE_G_FREE(current_colum)
    current_colum = temp;

    chart->rows = MAX(chart->rows, index_colum + 1);
    index_colum++;
  }

out:
  result = TRUE;
  goto end;

error:
  fprintf(stderr, "error parsing CHT file, in %s %s:%d\n", __FUNCTION__, __FILE__, lineno);

end:
  SAFE_G_FREE(last_label)
  SAFE_G_FREE(current_row)
  SAFE_G_FREE(current_colum)
  return result;
}

/**
 * @brief Parses a CHT file and extracts the boxes data.
 * 
 * @param filename The path to the CHT file to parse.
 * @return GList* A GList containing the parsed boxes data. Each element is a GList of strings representing a box.
 */
static GList *_parse_cht(const char *filename)
{
  GList *result = NULL;

  int lineno = 0;
  GIOChannel *fp = g_io_channel_new_file(filename, "r", NULL);
  if(!fp)
  {
    fprintf(stderr, "Error opening '%s'\n", filename);
    return NULL;
  }

  // parser control
  GString *line = g_string_new(NULL);
  parser_state_t last_block = BLOCK_NONE;
  int skip_block = 0;
  
  // main loop over the input file
  while(g_io_channel_read_line_string(fp, line, NULL, NULL) == G_IO_STATUS_NORMAL)
  {
    if(line->len == 0)
    {
      skip_block = 0;
      continue;
    }
    if(skip_block) continue;

    // we should be at the start of a block now
    const char *c = line->str;
    while(*c == ' ') c++; // skip leading spaces
    gchar **line_tokens = g_strsplit(c, " ", 0);

    if(!g_strcmp0(line_tokens[0], "BOXES") && last_block < BLOCK_BOXES)
    {
      last_block = BLOCK_BOXES;

      // let's have another loop reading from the file.
      while(g_io_channel_read_line_string(fp, line, NULL, NULL) == G_IO_STATUS_NORMAL)
      {
        if(line->len == 0) break;

        c = line->str;
        while(*c == ' ') c++; // skip leading spaces

        gchar **box_tokens = g_strsplit(c, " ", 0);
        if(!g_strcmp0(box_tokens[0], "F") || !g_strcmp0(box_tokens[0], "D") || !g_strcmp0(box_tokens[0], "X") || !g_strcmp0(box_tokens[0], "Y"))
          result = g_list_append(result, box_tokens);
      }
    }

    if(!g_strcmp0(line_tokens[0], "BOX_SHRINK") && last_block < BLOCK_BOX_SHRINK)
    {
      last_block = BLOCK_BOX_SHRINK;
      skip_block = 1;
    }

    g_strfreev(line_tokens); 
  }

  if(last_block == BLOCK_NONE)
    ERROR

  goto end;

error:
  fprintf(stderr, "error parsing CHT file, in %s %s:%d\n", __FUNCTION__, __FILE__, lineno);

end:
  if(line) g_string_free(line, TRUE);
  if(fp) g_io_channel_unref(fp);
  return result;
}

// according to cht_format.html from argyll:
// "The keywords and associated data must be used in the following order: BOXES, BOX_SHRINK, REF_ROTATION,
// XLIST, YLIST and EXPECTED."
static gboolean _dispatch_cht_data(GList **boxes, dt_colorchecker_chart_spec_t *chart_spec)
{ 
  gboolean result = FALSE;
  int lineno = 0;

  // data gathered from the CHT file
  cht_box_F_t *F_box = NULL;
  GList *boxes_list = NULL;

  float chart_radius = -1.f;

  for(GList *lines = *boxes; lines; lines = g_list_next(lines))
  {
    const char **tokens = (const char **)lines->data;
    if(!tokens) ERROR

    const char letter = tokens[0][0];
    if(letter == 'F')
    {
      F_box = _dt_cht_extract_F(tokens);
    }
 
    else if(letter == 'D' || letter == 'X' || letter == 'Y')
    {
      cht_box_t *box = _dt_cht_box_extract(tokens);
      if(!box) ERROR

      boxes_list = g_list_append(boxes_list, box);
    }
  }

  if(!F_box) ERROR

  // Fill the colorchecker spec structure
  chart_spec->ratio = F_box->height / F_box->width;
  chart_radius = hypotf(F_box->height, F_box->width);

  for(GList *iter = boxes_list; iter; iter = g_list_next(iter))
  {
    cht_box_t *box = (cht_box_t *)iter->data;
    if(!box) ERROR

    if(box->key_letter == 'D')
    {
      // Save the guide corner sizes when they are specified, to changes the patches area size in consequence. 
      if(!g_strcmp0(box->label_x_start,"MARK")) chart_spec->guide_size[0] = box->width - box->x_origin;
      if(!g_strcmp0(box->label_x_start,"MARK")) chart_spec->guide_size[1] = box->height - box->y_origin;
    }

    else if(box->key_letter == 'X' || box->key_letter == 'Y')
    {
      chart_spec->patch_width  = MIN(chart_spec->patch_width, box->width);
      chart_spec->patch_height = MIN(chart_spec->patch_height, box->height);
      
      if(!_dt_cht_generate_patch_list(chart_spec, box, F_box))
      {
        free(box->label_x_start);
        free(box->label_x_end);
        free(box->label_y_start);
        free(box->label_y_end);
        free(box);
        ERROR
      }
    }
  }

  chart_spec->num_patches = g_slist_length(chart_spec->patches);
  chart_spec->size[0] = (size_t)chart_spec->colums;
  chart_spec->size[1] = (size_t)chart_spec->rows;
  const float patch_radius = hypotf(chart_spec->patch_width, chart_spec->patch_height) / TWO_SQRT2f;
  chart_spec->radius = patch_radius / chart_radius;

  result = TRUE;
  goto end;

error:
  fprintf(stderr, "Error dispatching CHT file, in %s %s:%d\n", __FUNCTION__, __FILE__, lineno);

end:
  SAFE_FREE(F_box)
  if(boxes_list) g_list_free_full(boxes_list, _dt_cht_box_cleanup);

  return result;
}

/**
 * @brief Opens a CHT file and parses its content to fill the chart_spec structure. 
 * 
 * @param filename The path to the CHT file.
 * @param chart_spec The initialized structure to fill with the parsed data.
 * @return gboolean TRUE if the file was successfully parsed and the chart_spec filled, FALSE otherwise.
 */
static gboolean _dt_colorchecker_open_cht(const char *filename, dt_colorchecker_chart_spec_t *chart_spec)
{
  GList *boxes = _parse_cht(filename);
  if(!boxes)
  {
    fprintf(stderr, "Error parsing CHT file '%s'\n", filename);
    return FALSE;
  }

  if(!_dispatch_cht_data(&boxes, chart_spec))
  {
    fprintf(stderr, "Error dispatching CHT data from '%s'\n", filename);
    g_list_free_full(boxes, (GDestroyNotify)g_strfreev);
    return FALSE;
  }

  chart_spec->type = g_path_get_basename(filename);

  g_list_free_full(boxes, (GDestroyNotify)g_strfreev);

  return TRUE;
}

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

/**
 * @brief Get the standard type name from a CGATS type.
 * 
 * @param type the CGATS type (the first 7 characters of the CGATS file)
 * @return gchar* The standard type name, or "Unknown Type" if the type is invalid.
 */
static gchar *_dt_colorchecker_get_standard_type(const char *type)
{
  gchar *result = NULL;

  if(!type)
  {
    fprintf(stderr, "Error: Invalid CGATS type provided.\n");
    result = g_strdup("Unknown Type");
  }
  else
  {
    dt_colorchecker_CGATS_types t = _dt_CGATS_get_type_value(type);
    if(t == CGATS_TYPE_IT8_7_1 || t == CGATS_TYPE_IT8_7_2)
      result = g_strdup("IT8"); // make a shorter title for the IT8 types
    
    else if(t > CGATS_TYPE_IT8_7_2 && t < CGATS_TYPE_UNKOWN)
      result = g_strdup(CGATS_types[t]);
    
    else
    {
      fprintf(stderr, "Warning: Unknown CGATS type: %s\n", type);
      result = g_strdup(type);
    }
  }
  
  return result;
}

/**
 * @brief Test if the file is a CGATS.17 file
 * and if it contains one table of patch only.
 *
 * @param hIT8 pointer to the cmsHANDLE
 * @return gboolean TRUE if the file is valid, FALSE otherwise.
 */
static gboolean _dt_CGATS_is_supported(const cmsHANDLE *hIT8)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error loading IT8 file.\n");
    return FALSE;
  }
  else
  {    
    const char *CGATS_type = cmsIT8GetProperty(*hIT8, "CGATS");
    // Check if the data type can be found in our supported list of CGATS types
    if(_dt_CGATS_get_type_value(CGATS_type) == CGATS_TYPE_UNKOWN)
    {
      fprintf(stderr, "Warning: type '%s' is not supported by Ansel.\n", CGATS_type);
      return FALSE;
    }

    int column_SAMPLE_ID = -1;
    int column_X = -1;
    int column_Y = -1;
    int column_Z = -1;
    int column_L = -1;
    int column_a = -1;
    int column_b = -1;
    char **sample_names = NULL;
    int n_columns = cmsIT8EnumDataFormat(*hIT8, &sample_names);

    if(n_columns == -1)
    {
      fprintf(stderr, "Error with the CGATS file, can't get column types\n");
      return FALSE;
    }

    for(int i = 0; i < n_columns; i++)
    {
      if(!g_strcmp0(sample_names[i], "SAMPLE_ID") || !g_strcmp0(sample_names[i], "SAMPLE_LOC"))
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
      fprintf(stderr, "Error: can't find the SAMPLE_ID column in the CGATS file.\n");
      return FALSE;
    }

    if(column_X + column_Y + column_Z + column_L + column_a + column_b == -1)
    {
      fprintf(stderr, "Error: No XYZ or Lab columns found in the CGATS file.\n");
      return FALSE;
    }

    uint32_t table_count = cmsIT8TableCount(*hIT8);
    if(table_count != 1)
    {
      fprintf(stderr, "Warning: the CGATS file contains %u tables but we only support files"
              "with one table at the moment.\n", table_count);
      return FALSE;
    }
  }

  return TRUE;
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
static inline const char *_dt_get_builtin_colorchecker_name(const dt_color_checker_targets target_type)
{
  dt_color_checker_t *color_checker = dt_get_color_checker(target_type, NULL, NULL);
  if(!color_checker)
  {
    fprintf(stderr, "Error: Unable to get the color checker %d.\n", target_type);
    return NULL;
  }
  const char *name = g_strdup(color_checker->name);
  
  dt_colorchecker_cleanup(color_checker);
  return name;
}

/**
 * @brief Get the number of patches in a built-in colorchecker.
 * 
 * @param target_type The type of colorchecker
 * @return int The number of patches in the colorchecker, or 0 if an error occurred.
 */
static inline int _dt_get_builtin_colorchecker_patch_nb(const dt_color_checker_targets target_type)
{
  dt_color_checker_t *color_checker = dt_get_color_checker(target_type, NULL, NULL);
  if(!color_checker)
  {
    fprintf(stderr, "Error: Unable to get the color checker %d.\n", target_type);
    return 0;
  }
  const int patch_nb = color_checker->patches;
  
  dt_colorchecker_cleanup(color_checker);
  return patch_nb;
}

/**
 * @brief build a name for the colorchecker.
 * The returned string must be freed by the caller.
 *
 * @param label the struct containing useful data to build a label
 * @return gchar* String with the name of the colorchecker.
 */
static gchar *_dt_colorchecker_label_build_name(const dt_colorchecker_CGATS_label_make_name_t *label)
{  
  const gchar *type = label->type ? label->type : g_strdup("?");
  // material if any
  gchar *tmp_material = (label->material && g_strcmp0(label->material, "") != 0) ? g_strdup_printf(" (%s)", label->material) : g_strdup("");
  // Description if any
  gchar *tmp_description = (label->description && g_strcmp0(label->description, "") != 0) ? g_strdup(label->description) : g_strdup("Unknown");
  
  // Compose: filename
  gchar *name = g_strdup_printf("%s%s - %s", type, tmp_material, tmp_description);

  // Clean up
  SAFE_G_FREE(tmp_material)
  SAFE_G_FREE(tmp_description)

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
  gchar *basename = NULL;

  if(filename && g_strcmp0(filename, "") != 0)
  {
    basename = g_path_get_basename(filename);
    char *dot = g_strrstr(basename, ".");
    if(dot)
    {
      // remove the file extension
      *dot = '\0';
    }
  }

  if(!hIT8)
  {
    fprintf(stderr, "dt_CGATS_get_name: Error: Invalid CGATS handle provided.\n");
    result = g_strdup(basename ? basename : "Unnamed CGATS");
  }
  else
  {
    // Get other useful information from the CGATS file
    gchar *chart_type = _dt_colorchecker_get_standard_type(cmsIT8GetSheetType(*hIT8));
    const char *description = cmsIT8GetProperty(*hIT8, "DESCRIPTOR");
    const dt_colorchecker_material_types material = _dt_colorchecker_IT8_get_material_type(hIT8);
    gchar *material_str = g_strdup(_dt_colorchecker_get_material_string(material));

    const dt_colorchecker_CGATS_label_make_name_t label = {
                                                      .type = chart_type,
                                                      .description = description ? description : NULL,
                                                      .material = material_str }; //can be NULL

    gchar *name = _dt_colorchecker_label_build_name(&label);
    
    if(name && g_strcmp0(name, "") != 0)
      result = name;
    else
      result = (basename && g_strcmp0(basename, "")) ? g_strdup(basename) : g_strdup("Unnamed CGATS");
    
    SAFE_G_FREE(chart_type)
    SAFE_G_FREE(material_str)
  }

  SAFE_G_FREE(basename)

  return result;
}

/**
 * @brief Get the number of patches in a CHT file.
 * 
 * @param filepath the path to the CHT file
 * @return int the number of patches in the CHT file, or 0 if an error occurred.
 */
static int _dt_colorchecker_cht_get_patch_nb(const char *filepath)
{
  int result = 0;

  dt_colorchecker_chart_spec_t *chart_spec = _dt_colorchecker_chart_spec_init();
  if(!chart_spec) 
  {
    fprintf(stderr, "Error: cannot allocate memory for the chart spec.\n");
    goto end;
  }

  if(!_dt_colorchecker_open_cht(filepath, chart_spec))
  {
    fprintf(stderr, "Error: cannot open the cht file '%s'.\n", filepath);
    goto end;
  }

  if(!chart_spec->num_patches)  
    fprintf(stderr, "Error: no patches found in the cht file '%s'.\n", filepath);
  else result = chart_spec->num_patches;

  end:
  _dt_colorchecker_chart_spec_cleanup(chart_spec);
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
 * @param bwg the array of indices for the black, white, and grey patches
 * @param chart_spec the color checker chart specification, used to get the number of patches and patch size
 * @param num_patches the number of patches to fill, should be the minimum between the CGATS file and the chart specification
 * @return dt_color_checker_patch* a pointer to the array of patches filled with values, or NULL on error.
 */
static dt_color_checker_patch *_dt_colorchecker_CGATS_fill_patch_values(const cmsHANDLE hIT8, size_t *bwg, const dt_colorchecker_chart_spec_t *chart_spec, const size_t num_patches)
{
  int column_SAMPLE_ID = -1;
  int column_X = -1;
  int column_Y = -1;
  int column_Z = -1;
  int column_L = -1;
  int column_a = -1;
  int column_b = -1;
  char **sample_names = NULL;
  int n_columns = cmsIT8EnumDataFormat(hIT8, &sample_names);

  dt_color_checker_patch *values = dt_colorchecker_patch_array_init(num_patches);
  if(!values)
  {
    fprintf(stderr, "Error: Memory allocation failed for values array.\n");
    goto error;
  }

  gboolean use_XYZ = FALSE;
  if(n_columns == -1)
  {
    fprintf(stderr, "Error with the CGATS file, can't get column types\n");
  }

  for(int i = 0; i < n_columns; i++)
  {
    if(!g_strcmp0(sample_names[i], "SAMPLE_ID") || !g_strcmp0(sample_names[i], "SAMPLE_LOC"))
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
    fprintf(stderr, "Error: can't find the SAMPLE_ID column in the CGATS file.\n");
    goto error;
  }

  if(column_X + column_Y + column_Z + column_L + column_a + column_b == -1)
  {
    fprintf(stderr, "Error: No XYZ or Lab columns found in the CGATS file.\n");
    goto error;
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
    fprintf(stderr, "Error: can't find XYZ or Lab columns in the CGATS file\n");
    goto error;
  }

  for(size_t patch_iter = 0; patch_iter < num_patches; patch_iter++)
  {
    // set name
    values[patch_iter].name = g_strdup(cmsIT8GetDataRowCol(hIT8, patch_iter, 0));
    if(values[patch_iter].name == NULL)
    {
      fprintf(stderr, "Error : can't find sample '%lu' in CGATS file\n", patch_iter);
      goto error;
    }
    
    // set patch position
    // The position of the patch is given by the chart specification
    const dt_color_checker_patch *p = (dt_color_checker_patch*)g_slist_nth_data(chart_spec->patches, (guint)patch_iter);
    if(!p)
    {
      fprintf(stderr, "Error: patch %lu not found in chart specification.\n", patch_iter);
      goto error;
    }
    _dt_colorchecker_copy_patch(&values[patch_iter], p);      
    
    // Copy color values
    const double patchdbl[3] = { cmsIT8GetDataRowColDbl(hIT8, (int)patch_iter, columns[0]),
                                 cmsIT8GetDataRowColDbl(hIT8, (int)patch_iter, columns[1]),
                                 cmsIT8GetDataRowColDbl(hIT8, (int)patch_iter, columns[2]) };

    // Convert to Lab when it's in XYZ
    if(use_XYZ)
    {
      const dt_aligned_pixel_t patch_color = { (float)patchdbl[0] * 0.01, (float)patchdbl[1] *0.01, (float)patchdbl[2] * 0.01, 0.0f };
      dt_XYZ_to_Lab(patch_color, values[patch_iter].Lab);
    }
    else
    {
      values[patch_iter].Lab[0] = (float)patchdbl[0];
      values[patch_iter].Lab[1] = (float)patchdbl[1];
      values[patch_iter].Lab[2] = (float)patchdbl[2];
    }

    _dt_CGATS_find_whitest_blackest_greyest(values, bwg, patch_iter);
  }

  goto end;

error:
  if(values)
  {
    for(size_t i = 0; i < num_patches; i++)
    { 
      dt_colorchecker_patch_cleanup(&values[i]);
    }
  }
  values = NULL;

end:
  return values;
}

dt_color_checker_t *dt_colorchecker_user_ref_create(const char *color_filename, const char *cht_filename)
{
  dt_colorchecker_chart_spec_t *chart_spec = NULL;
  dt_color_checker_t *checker = NULL;

  int lineno = 0;

  if(!g_file_test(color_filename, G_FILE_TEST_IS_REGULAR))
  {
    fprintf(stderr, "Error: the color file '%s' does not exist or is not a regular file.\n", color_filename);
    return NULL;
  }
  
  cmsHANDLE hIT8 = cmsIT8LoadFromFile(NULL, color_filename);

  if(!_dt_CGATS_is_supported(&hIT8))
  {
    fprintf(stderr, "Ansel cannot load the CGATS file '%s'\n", color_filename);
    ERROR
  }

  chart_spec = _dt_colorchecker_chart_spec_init();
  if(!chart_spec)
  {
    fprintf(stderr, "Error: cannot allocate memory for the chart spec.\n");
    ERROR
  }
  // load the cht file if any
  if(cht_filename && g_file_test(cht_filename, G_FILE_TEST_IS_REGULAR))
  {
    if(!_dt_colorchecker_open_cht(cht_filename, chart_spec))
    {
      fprintf(stderr, "Error: cannot open the cht file '%s'.\n", cht_filename);
      ERROR
    }
  }
  else fprintf(stderr, "Warning: invalid cht file '%s'.\n", cht_filename);

  // Check if the CGATS file contains the expected number of patches
  const int num_patches_it8 = (const int)cmsIT8GetPropertyDbl(hIT8, "NUMBER_OF_SETS");
  
  if(chart_spec->num_patches > 0 && num_patches_it8 != chart_spec->num_patches)
  {
    fprintf(stderr, "Warning: the number of patches in the CGATS file (%i) does not match the expected number (%i) in the cht file.\n",
            num_patches_it8, chart_spec->num_patches);
  }

  // Limit the number of patches to the minimum between the CGATS file and the chart specification to avoid overflow.
  const size_t num_patches = MIN(num_patches_it8, chart_spec->num_patches);
  dt_print(DT_DEBUG_VERBOSE, "%zu patches will be added to the chart\n", num_patches);

  checker = dt_colorchecker_init();
  if(!checker)
  {
    fprintf(stderr, "Error: cannot allocate memory for the color checker.\n");
    ERROR
  }

  checker->name = _dt_CGATS_get_name(&hIT8, color_filename);
  checker->author = g_strdup(_dt_CGATS_get_author(&hIT8));
  checker->date = g_strdup(_dt_CGATS_get_date(&hIT8));
  checker->manufacturer = g_strdup(_dt_CGATS_get_manufacturer(&hIT8));
  checker->type = COLOR_CHECKER_USER_REF;
  checker->radius = chart_spec->radius;
  checker->ratio = chart_spec->ratio;
  checker->patches = num_patches;
  checker->size[0] = chart_spec->size[0];
  checker->size[1] = chart_spec->size[1];
  checker->middle_grey = chart_spec->middle_grey;
  checker->white = chart_spec->white;
  checker->black = chart_spec->black;

  // blackest, whitest and greyest patches will be found while filling the color values
  size_t bwg[3] = { 0, 0, 0 };
  checker->values = _dt_colorchecker_CGATS_fill_patch_values(hIT8, bwg, chart_spec, num_patches);
  if(!checker->values)
  {
    fprintf(stderr, "Error: cannot fill the color values from the CGATS file.\n");
    ERROR
  }

  checker->black = bwg[0];
  checker->white = bwg[1];
  checker->middle_grey = bwg[2];
  dt_print(DT_DEBUG_VERBOSE, _("blackest patch: %s, middle grey patch: %s, white patch: %s\n"),
           checker->values[bwg[0]].name, checker->values[bwg[1]].name, checker->values[bwg[2]].name);

  dt_print(DT_DEBUG_VERBOSE, _("it8 '%s' done\n"), color_filename);
  goto end;

  error:
  fprintf(stderr, "Error creating user ref checker, in %s %s:%d\n", __FUNCTION__, __FILE__, lineno);

  end:
  if(chart_spec) _dt_colorchecker_chart_spec_cleanup(chart_spec); // only allocated chart will be freed
  if(hIT8) cmsIT8Free(hIT8);
  return checker;
}

static dt_colorchecker_label_t *_dt_colorchecker_user_ref_make_label(const gchar *filename, const gchar *user_it8_dir)
{
  dt_colorchecker_label_t *result = NULL;

  gchar *filepath = g_build_filename(user_it8_dir, filename, NULL);
  if(g_file_test(filepath, G_FILE_TEST_IS_REGULAR))
  {
    cmsHANDLE hIT8 = cmsIT8LoadFromFile(NULL, filepath);
    const int patch_nb = (int)cmsIT8GetPropertyDbl(hIT8, "NUMBER_OF_SETS");
    if(hIT8 && _dt_CGATS_is_supported(&hIT8) && patch_nb > 0)
    {
      gchar *label = _dt_CGATS_get_name(&hIT8, filename);
      dt_colorchecker_label_t *CGATS_label = dt_colorchecker_label_init(label, COLOR_CHECKER_USER_REF, filepath, patch_nb);
          
      SAFE_G_FREE(label);
      result = CGATS_label;
    }
    cmsIT8Free(hIT8);
  }
  SAFE_G_FREE(filepath)

  if(!result)
  {
    SAFE_FREE(result)
    return NULL;
  }
  else return result;
}

static dt_colorchecker_label_t *_dt_colorchecker_cht_make_label(const gchar *filename, const gchar *user_it8_dir)
{
  dt_colorchecker_label_t *cht_label = NULL;

  gchar *filepath = g_build_filename(user_it8_dir, filename, NULL);
  if(g_file_test(filepath, G_FILE_TEST_IS_REGULAR))
  {
    gchar *basename = g_path_get_basename(filename);
    char *dot = g_strrstr(basename, ".");
    if(dot) *dot = '\0'; // removes the file extension in basename
    const int patch_nb = _dt_colorchecker_cht_get_patch_nb(filepath);
    
    if(patch_nb > 0) // only create a label if the CHT file has patches
      cht_label = dt_colorchecker_label_init(basename, COLOR_CHECKER_USER_REF, filepath, patch_nb);

    SAFE_G_FREE(basename)
  }
  SAFE_G_FREE(filepath)

  return cht_label;
}

int dt_colorchecker_find_builtin(GList **colorcheckers_label)
{
  int nb = 0;
  for(int k = 0; k < COLOR_CHECKER_USER_REF; k++)
  {
    const char *name = _dt_get_builtin_colorchecker_name(k);
    const int patch_nb = _dt_get_builtin_colorchecker_patch_nb(k);
    if(patch_nb <= 0) continue; // skip color checkers with no patches
    
    dt_colorchecker_label_t *builtin_label = dt_colorchecker_label_init(name, k, NULL, patch_nb);

    if(!builtin_label)
    {
      fprintf(stderr, "Error: failed to allocate memory for builtin colorchecker label %d\n", k);
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

int dt_colorchecker_find_CGATS_reference_files(GList **ref_colorcheckers_files)
{
  int nb = 0;
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  gchar *user_it8_dir = g_build_filename(confdir, "color", "checker", NULL);

  GDir *dir = g_dir_open(user_it8_dir, 0, NULL);
  if(dir)
  {
    const char *filename;
    while((filename = g_dir_read_name(dir)) != NULL)
    {
      const char *dot = g_strrstr(filename, ".");
      if(g_ascii_strcasecmp(dot, ".cht") == 0)
        continue; // skip .cht files
      
      dt_colorchecker_label_t *CGATS_label = _dt_colorchecker_user_ref_make_label(filename, user_it8_dir);
      if(CGATS_label)
      {
        *ref_colorcheckers_files = g_list_append(*ref_colorcheckers_files, CGATS_label);
        nb++;
      }
      else
        fprintf(stderr, "Warning: failed to load CGATS file '%s' in %s\n", filename, user_it8_dir);
    }
    g_dir_close(dir);
  }
  SAFE_G_FREE(user_it8_dir)

  return nb;
}

int dt_colorchecker_find_cht_files(GList **chts)
{
  int nb = 0;
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  gchar *user_it8_dir = g_build_filename(confdir, "color", "checker", NULL);

  GDir *dir = g_dir_open(user_it8_dir, 0, NULL);
  if(dir)
  {
    const char *filename;
    while((filename = g_dir_read_name(dir)) != NULL)
    {
      const char *dot = g_strrstr(filename, ".");
      if(g_ascii_strcasecmp(dot, ".cht") != 0)
        continue; // skip files that are not .cht

      dt_colorchecker_label_t *cht_label = _dt_colorchecker_cht_make_label(filename, user_it8_dir);
      if(cht_label)
      {
        *chts = g_list_append(*chts, cht_label);
        nb++;
      }
    }
    g_dir_close(dir);
  }
  SAFE_G_FREE(user_it8_dir)

  return nb;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
