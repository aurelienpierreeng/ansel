/*
    This file is part of darktable,
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019 Jakub Filipowicz.
    Copyright (C) 2019-2022 Pascal Obry.
    Copyright (C) 2019-2021 Philippe Weyland.
    Copyright (C) 2019 Tobias Ellinghaus.
    Copyright (C) 2020 Aldric Renaudin.
    Copyright (C) 2020, 2023-2026 Aurélien PIERRE.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020 David-Tillmann Schaefer.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Marco.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021-2022 Hanno Schwalm.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2023 Maurizio Paglia.
    Copyright (C) 2024 Alynx Zhou.
    
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
#ifdef HAVE_CONFIG_H
#include "caches/pixelpipe_cache_alloc.h"
#include "common/conf.h"
#include "config.h"
#endif

#include "widgets/bauhaus.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/module_versioning.h"
#include <glib/gstdio.h>
#include "imageio/imageio_png.h"
#include "common/imagebuf.h"
#include "colorprofiles/colorspaces.h"
#include "common/file_location.h"
#include "develop/iop_profile.h"
#include "pixel/lut3d.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/develop.h"
#include "widgets/button.h"
#include "widgets/dialog.h"
#include "gui/application.h"

#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include "widgets/widget_style.h"
#include "control/signal.h"
#if defined (_WIN32)
#include "win/getdelim.h"
#endif // defined (_WIN32)

DT_MODULE_INTROSPECTION(3, dt_iop_lut3d_params_t)

#define DT_IOP_LUT3D_MAX_PATHNAME 512
#define DT_IOP_LUT3D_MAX_LUTNAME 128
#define DT_IOP_LUT3D_CLUT_LEVEL 48
#define DT_IOP_LUT3D_MAX_KEYPOINTS 2048

typedef enum dt_iop_lut3d_colorspace_t
{
  DT_IOP_SRGB = 0,    // $DESCRIPTION: "sRGB"
  DT_IOP_ARGB,        // $DESCRIPTION: "Adobe RGB"
  DT_IOP_REC709,      // $DESCRIPTION: "gamma Rec709 RGB"
  DT_IOP_LIN_REC709,  // $DESCRIPTION: "linear Rec709 RGB"
  DT_IOP_LIN_REC2020, // $DESCRIPTION: "linear Rec2020 RGB"
  DT_IOP_ITUR_BT1886, // $DESCRIPTION: "ITU-R BT.1886 (gamma 2.4 Rec709)"
} dt_iop_lut3d_colorspace_t;

typedef enum dt_iop_lut3d_interpolation_t
{
  DT_IOP_TETRAHEDRAL = 0, // $DESCRIPTION: "tetrahedral"
  DT_IOP_TRILINEAR = 1,   // $DESCRIPTION: "trilinear"
  DT_IOP_PYRAMID = 2,     // $DESCRIPTION: "pyramid"
} dt_iop_lut3d_interpolation_t;

typedef struct dt_iop_lut3d_params_t
{
  char filepath[DT_IOP_LUT3D_MAX_PATHNAME];
  dt_iop_lut3d_colorspace_t colorspace; // $DEFAULT: DT_IOP_SRGB $DESCRIPTION: "application color space"
  dt_iop_lut3d_interpolation_t interpolation; // $DEFAULT: DT_IOP_TETRAHEDRAL
  int nb_keypoints; // $DEFAULT: 0 >0 indicates the presence of compressed lut
  char c_clut[DT_IOP_LUT3D_MAX_KEYPOINTS*2*3];
  char lutname[DT_IOP_LUT3D_MAX_LUTNAME];
} dt_iop_lut3d_params_t;

typedef struct dt_iop_lut3d_gui_data_t
{
  // Whether the deprecation dialog has been shown yet, and for which path, so gui_update() --
  // which runs on every history change -- raises it once per affected LUT rather than repeatedly.
  // The flag is separate from the path on purpose: an edit that carries keypoints inside the
  // history has no filepath at all, and testing the path alone would compare "" against the
  // zero-initialised buffer, match, and silently skip the warning for precisely the oldest edits.
  gboolean gmz_warned;
  char gmz_warned_for[DT_IOP_LUT3D_MAX_PATHNAME];
  GtkWidget *hbox;
  GtkWidget *filepath;
  GtkWidget *colorspace;
  GtkWidget *interpolation;
} dt_iop_lut3d_gui_data_t;

typedef enum dt_lut3d_cols_t
{
  DT_LUT3D_COL_NAME = 0,
  DT_LUT3D_COL_VISIBLE,
  DT_LUT3D_NUM_COLS
} dt_lut3d_cols_t;

const char invalid_filepath_prefix[] = "INVALID >> ";

typedef struct dt_iop_lut3d_data_t
{
  dt_iop_lut3d_params_t params;
  float *clut;  // cube lut pointer
  uint16_t level; // cube_size
} dt_iop_lut3d_data_t;

typedef struct dt_iop_lut3d_global_data_t
{
  int kernel_lut3d_tetrahedral;
  int kernel_lut3d_trilinear;
  int kernel_lut3d_pyramid;
  int kernel_lut3d_none;
} dt_iop_lut3d_global_data_t;


const char *name()
{
  return _("lut 3D");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("perform color space corrections and apply look"),
                                      _("corrective or creative"),
                                      _("linear, RGB, display-referred"),
                                      _("defined by profile, RGB"),
                                      _("linear or non-linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_iop_lut3d_params_v1_t
    {
      char filepath[DT_IOP_LUT3D_MAX_PATHNAME];
      int colorspace;
      int interpolation;
    } dt_iop_lut3d_params_v1_t;

    dt_iop_lut3d_params_v1_t *o = (dt_iop_lut3d_params_v1_t *)old_params;
    dt_iop_lut3d_params_t *n = (dt_iop_lut3d_params_t *)new_params;
    g_strlcpy(n->filepath, o->filepath, sizeof(n->filepath));
    n->colorspace = o->colorspace;
    n->interpolation = o->interpolation;
    n->nb_keypoints = 0;
    memset(&n->c_clut, 0, sizeof(n->c_clut));
    memset(&n->lutname, 0, sizeof(n->lutname));
    return 0;
  }
  if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_iop_lut3d_params_v2_t
    {
      char filepath[DT_IOP_LUT3D_MAX_PATHNAME];
      int colorspace;
      int interpolation;
      int nb_keypoints; // >0 indicates the presence of compressed lut
      char c_clut[DT_IOP_LUT3D_MAX_KEYPOINTS*2*3];
      char lutname[DT_IOP_LUT3D_MAX_LUTNAME];
      uint32_t gmic_version;
    } dt_iop_lut3d_params_v2_t;

    dt_iop_lut3d_params_v2_t *o = (dt_iop_lut3d_params_v2_t *)old_params;
    dt_iop_lut3d_params_t *n = (dt_iop_lut3d_params_t *)new_params;
    memcpy(n, o, sizeof(dt_iop_lut3d_params_t));
    return 0;
  }

  return 1;
}
uint16_t calculate_clut_haldclut(dt_iop_lut3d_params_t *const p, const char *const filepath, float **clut)
{
  dt_imageio_png_t png;
  if(read_header(filepath, &png))
  {
    fprintf(stderr, "[lut3d] invalid png file %s\n", filepath);
    dt_control_log(_("invalid png file %s"), filepath);
    return 0;
  }
  dt_print(DT_DEBUG_DEV, "[lut3d] png: width=%d, height=%d, color_type=%d, bit_depth=%d\n", png.width,
           png.height, png.color_type, png.bit_depth);
  if (png.bit_depth !=8 && png.bit_depth != 16)
  {
    fprintf(stderr, "[lut3d] png bit-depth %d not supported\n", png.bit_depth);
    dt_control_log(_("png bit-depth %d not supported"), png.bit_depth);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }

  // check the file sizes
  uint16_t level = 2;
  while(level * level * level < png.width) ++level;

  if(level * level * level != png.width)
  {
    if (png.height == 2)
    {
      fprintf(stderr, "[lut3d] this Ansel build is not compatible with compressed clut\n");
      dt_control_log(_("this Ansel build is not compatible with compressed clut"));
    }
    else
    {
      fprintf(stderr, "[lut3d] invalid level in png file %d %d\n", level, png.width);
      dt_control_log(_("invalid level in png file %d %d"), level, png.width);
    }
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }

  level *= level;  // to be equivalent to cube level
  if(level > 256)
  {
    fprintf(stderr, "[lut3d] error - LUT 3D size %d > 256\n", level);
    dt_control_log(_("error - lut 3D size %d exceeds the maximum supported"), level);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
  const size_t buf_size = (size_t)png.height * png_get_rowbytes(png.png_ptr, png.info_ptr);
  dt_print(DT_DEBUG_DEV, "[lut3d] allocating %" G_GSIZE_FORMAT " bytes for png file\n", buf_size);
  uint8_t *buf = NULL;
  buf = dt_pixelpipe_cache_alloc_align_cache(buf_size, 0);
  if(IS_NULL_PTR(buf))
  {
    fprintf(stderr, "[lut3d] error allocating buffer for png lut\n");
    dt_control_log(_("error allocating buffer for png lut"));
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
  if (read_image(&png, buf))
  {
    fprintf(stderr, "[lut3d] error - could not read png image `%s'\n", filepath);
    dt_control_log(_("error - could not read png image %s"), filepath);
    dt_pixelpipe_cache_free_align(buf);
    return 0;
  }
  const size_t buf_size_lut = (size_t)png.height * png.height * 3;
  dt_print(DT_DEBUG_DEV, "[lut3d] allocating %" G_GSIZE_FORMAT " floats for png lut - level %d\n", buf_size_lut, level);
  float *lclut = dt_pixelpipe_cache_alloc_align_cache(sizeof(float) * buf_size_lut, 0);
  if(IS_NULL_PTR(lclut))
  {
    fprintf(stderr, "[lut3d] error - allocating buffer for png lut\n");
    dt_control_log(_("error - allocating buffer for png lut"));
    dt_pixelpipe_cache_free_align(buf);
    return 0;
  }
  // get clut values
  const float norm = 1.0f / (powf(2.f, png.bit_depth) - 1.0f);
  if (png.bit_depth == 8)
  {
    for (size_t i = 0; i < buf_size_lut; ++i)
      lclut[i] = (float)buf[i] * norm;
  }
  else
  {
    for (size_t i = 0; i < buf_size_lut; ++i)
      lclut[i] = (256.0f * (float)buf[2*i] + (float)buf[2*i+1]) * norm;
  }
  dt_pixelpipe_cache_free_align(buf);
  *clut = lclut;
  return level;
}

// provided by @rabauke, atof replaces strtod & sccanf which are locale dependent
double dt_atof(const char *str)
{
  if (strncmp(str, "nan", 3) == 0 || strncmp(str, "NAN", 3) == 0)
    return NAN;
  double integral_result = 0;
  double fractional_result = 0;
  double sign = 1;
  if (*str == '+')
  {
    str++;
    sign = +1;
  } else if (*str == '-')
  {
    str++;
    sign = -1;
  }
  if (strncmp(str, "inf", 3) == 0 || strncmp(str, "INF", 3) == 0)
    return sign * INFINITY;
  // search for end of integral part and parse from
  // right to left for numerical stability
  const char * istr_back = str;
  while (*str >= '0' && *str <= '9')
    str++;
  const char * istr_2 = str;
  double imultiplier = 1;
  while (istr_2 != istr_back)
  {
    --istr_2;
    integral_result += (*istr_2 - '0') * imultiplier;
    imultiplier *= 10;
  }
  if (*str == '.')
  {
    str++;
  // search for end of fractional part and parse from
  // right to left for numerical stability
    const char * fstr_back = str;
    while (*str >= '0' && *str <= '9')
      str++;
    const char * fstr_2 = str;
    double fmultiplier = 1;
    while (fstr_2 != fstr_back)
    {
      --fstr_2;
      fractional_result += (*fstr_2 - '0') * fmultiplier;
      fmultiplier *= 10;
    }
    fractional_result /= fmultiplier;
  }
  double result = sign * (integral_result + fractional_result);
  if (*str == 'e' || *str == 'E')
  {
    str++;
    double power_sign = 1;
    if (*str == '+')
    {
      str++;
      power_sign = +1;
    }
    else if (*str == '-')
    {
      str++;
      power_sign = -1;
    }
    double power = 0;
    while (*str >= '0' && *str <= '9')
    {
      power *= 10;
      power += *str - '0';
      str++;
    }
    if (power_sign > 0)
      result *= pow(10, power);
    else
      result /= pow(10, power);
  }
  return result;
}

// return max 3 tokens from the line (separator = ' ' and token length = 50)
// if nb tokens > 3, the 3rd one captures the last input
uint8_t parse_cube_line(char *line, char (*token)[50])
{
  const int max_token_len = 50;
  uint8_t i = 0;
  uint8_t c = 0;
  char *t = &token[0][0];
  char *l = line;

  while (*l != 0 && i < max_token_len)
  {
    if (*l == '#' || *l == '\n' || *l == '\r')
    { // end of useful part of the line
      if (i > 0)
      {
        *t = 0;
        c++;
        return c;
      }
      else
      {
        *t = 0;
        return c;
      }
    }
    if (*l == ' ' || *l == '\t')
    { // separator
      if (i > 0)
      {
        *t = 0;
        c++;
        i = 0;
        t = &token[c > 2 ? 2 : c][0];
      }
    }
    else
    { // capture info
      *t = *l;
      t++;
      i++;
    }
    l++;
    // sometimes the last lf is missing
    if (*l == 0)
    {
      *t = 0;
      c++;
      return c;
    }
  }
  token[0][max_token_len - 1] = 0;
  token[1][max_token_len - 1] = 0;
  token[2][max_token_len - 1] = 0;
  return c;
}

uint16_t calculate_clut_cube(const char *const filepath, float **clut)
{
  FILE *cube_file;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  char token[3][50];
  uint16_t level = 0;
  float *lclut = NULL;
  uint32_t i = 0;
  size_t buf_size = 0;
  uint32_t out_of_range_nb = 0;

  if(!(cube_file = g_fopen(filepath, "r")))
  {
    fprintf(stderr, "[lut3d] invalid cube file: %s\n", filepath);
    dt_control_log(_("error - invalid cube file: %s"), filepath);
    return 0;
  }
  while ((read = getline(&line, &len, cube_file)) != -1)
  {
    const uint8_t nb_token = parse_cube_line(line, token);
    if (nb_token)
    {
      if (token[0][0] == 'T') continue;
      else if (strcmp("DOMAIN_MIN", token[0]) == 0)
      {
        if (strtod(token[1], NULL) != 0.0f)
        {
          fprintf(stderr, "[lut3d] DOMAIN MIN <> 0.0 is not supported\n");
          dt_control_log(_("DOMAIN MIN <> 0.0 is not supported"));
          dt_pixelpipe_cache_free_align(lclut);
          dt_free(line);
          fclose(cube_file);
        }
      }
      else if (strcmp("DOMAIN_MAX", token[0]) == 0)
      {
        if (strtod(token[1], NULL) != 1.0f)
        {
          fprintf(stderr, "[lut3d] DOMAIN MAX <> 1.0 is not supported\n");
          dt_control_log(_("DOMAIN MAX <> 1.0 is not supported"));
          dt_pixelpipe_cache_free_align(lclut);
          dt_free(line);
          fclose(cube_file);
        }
      }
      else if (strcmp("LUT_1D_SIZE", token[0]) == 0)
      {
        fprintf(stderr, "[lut3d] 1D cube lut is not supported\n");
        dt_control_log(_("[1D cube lut is not supported"));
        dt_free(line);
        fclose(cube_file);
        return 0;
      }
      else if (strcmp("LUT_3D_SIZE", token[0]) == 0)
      {
        level = atoll(token[1]);
        if(level > 256)
        {
          fprintf(stderr, "[lut3d] error - LUT 3D size %d > 256\n", level);
          dt_control_log(_("error - lut 3D size %d exceeds the maximum supported"), level);
          dt_free(line);
          fclose(cube_file);
          return 0;
        }
        buf_size = level * level * level * 3;
        dt_print(DT_DEBUG_DEV, "[lut3d] allocating %" G_GSIZE_FORMAT " bytes for cube lut - level %d\n", buf_size, level);
        lclut = dt_pixelpipe_cache_alloc_align_cache(sizeof(float) * buf_size, 0);
        if(IS_NULL_PTR(lclut))
        {
          fprintf(stderr, "[lut3d] error - allocating buffer for cube lut\n");
          dt_control_log(_("error - allocating buffer for cube lut"));
          dt_free(line);
          fclose(cube_file);
          return 0;
        }
      }
      else if (nb_token == 3)
      {
        if (!level)
        {
          fprintf(stderr, "[lut3d] error - cube lut size is not defined\n");
          dt_control_log(_("error - cube lut size is not defined"));
          dt_free(line);
          fclose(cube_file);
          return 0;
        }
        for (int j=0; j < 3; j++)
        {
          lclut[i+j] = dt_atof(token[j]);
          if(isnan(lclut[i+j]))
          {
            fprintf(stderr, "[lut3d] error - invalid number line %d\n", (int)i/3);
            dt_control_log(_("error - cube lut invalid number line %d"), (int)i/3);
            dt_free(line);
            fclose(cube_file);
            return 0;
          }
          else if(lclut[i+j] < 0.0 || lclut[i+j] > 1.0)
            out_of_range_nb++;
        }
        i += 3;
      }
    }
  }
  if (i != buf_size || i == 0)
  {
    fprintf(stderr, "[lut3d] error - cube lut lines number %d is not correct, should be %d\n",
            (int)i/3, (int)buf_size/3);
    dt_control_log(_("error - cube lut lines number %d is not correct, should be %d"),
                   (int)i/3, (int)buf_size/3);
    dt_pixelpipe_cache_free_align(lclut);
    dt_free(line);
    fclose(cube_file);
    return 0;
  }
  if(out_of_range_nb)
  {
    fprintf(stderr, "[lut3d] warning - %d out of range values [0,1]\n", out_of_range_nb);
    dt_control_log(_("warning - cube lut %d out of range values [0,1]"), out_of_range_nb);
  }
  *clut = lclut;
  dt_free(line);
  fclose(cube_file);
  return level;
}

uint16_t calculate_clut_3dl(const char *const filepath, float **clut)
{
  FILE *cube_file;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  char token[3][50];
  uint16_t level = 0;
  float *lclut = NULL;
  int max_value = 0;
  uint32_t i = 0;
  size_t buf_size = 0;

  if(!(cube_file = g_fopen(filepath, "r")))
  {
    fprintf(stderr, "[lut3d] invalid 3dl file: %s\n", filepath);
    dt_control_log(_("error - invalid 3dl file: %s"), filepath);
    return 0;
  }
  while ((read = getline(&line, &len, cube_file)) != -1)
  {
    const uint8_t nb_token = parse_cube_line(line, token);
    if (nb_token)
    {
      if (!level)
      {
        if (nb_token > 3)
        {
          // we assume the shaper is linear and gives the size of the cube (level)
          const int min_shaper = atoll(token[0]);
          const int max_shaper = atoll(token[2]);
          if (max_shaper > min_shaper)
          {
            level = nb_token; // max nb_token = 50 < 256
            if(max_shaper < 128)
            {
              fprintf(stderr, "[lut3d] error - the maximum shaper lut value %d is too low\n", max_shaper);
              dt_control_log(_("error - the maximum shaper lut value %d is too low"), max_shaper);
              dt_free(line);
              fclose(cube_file);
              return 0;
            }
            buf_size = level * level * level * 3;
            dt_print(DT_DEBUG_DEV, "[lut3d] allocating %" G_GSIZE_FORMAT " bytes for cube lut - level %d\n", buf_size, level);
            lclut = dt_pixelpipe_cache_alloc_align_cache(sizeof(float) * buf_size, 0);
            if(IS_NULL_PTR(lclut))
            {
              fprintf(stderr, "[lut3d] error - allocating buffer for cube lut\n");
              dt_control_log(_("error - allocating buffer for cube lut"));
              dt_free(line);
              fclose(cube_file);
              return 0;
            }
          }
        }
      }
      else if (nb_token == 3)
      {
        if (!level)
        {
          fprintf(stderr, "[lut3d] error - cube lut size is not defined\n");
          dt_control_log(_("error - cube lut size is not defined"));
          dt_free(line);
          fclose(cube_file);
          return 0;
        }
        // indexing starts with blue instead of red. need to restore the right index
        const uint32_t level2 = level * level;
        const uint32_t red = i / level2;
        const uint32_t rr = i - red * level2;
        const uint32_t green = rr / level;
        const uint32_t blue = rr - green * level;
        const uint32_t k = red + level * green + level2 * blue;
        for (int j=0; j < 3; j++)
        {
          const uint32_t value = atoll(token[j]);
          lclut[k*3+j] = (float)value;
          if (value > max_value)
            max_value = value;
        }
        i++;
        if (i * 3 > buf_size)
          break;
      }
    }
  }
  if (i * 3 != buf_size || i == 0)
  {
    fprintf(stderr, "[lut3d] error - cube lut lines number is not correct\n");
    dt_control_log(_("error - cube lut lines number is not correct"));
    dt_pixelpipe_cache_free_align(lclut);
    dt_free(line);
    fclose(cube_file);
    return 0;
  }
  dt_free(line);
  fclose(cube_file);

  // search bit depth: min 2^x > max_value
  int inorm = 1;
  while ((inorm < max_value) && (inorm < 65536))  // bit depth 16
    inorm <<= 1;
  if (inorm < 128)  // bit depth 7
  {
    fprintf(stderr, "[lut3d] error - the maximum lut value does not match any valid bit depth\n");
    dt_control_log(_("error - the maximum lut value does not match any valid bit depth"));
    dt_pixelpipe_cache_free_align(lclut);
    return 0;
  }
  const float norm = 1.0f / (float)(inorm - 1);
  // normalize the lut
  for (i =0; i < buf_size; i++)
    lclut[i] = CLAMP(lclut[i] * norm, 0.0f, 1.0f);
  *clut = lclut;
  return level;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  dt_iop_lut3d_global_data_t *gd = (dt_iop_lut3d_global_data_t *)self->global_data;
  cl_int err = CL_SUCCESS;
  const float *const clut = (float *)d->clut;
  const int level = d->level;
  const int kernel = (d->params.interpolation == DT_IOP_TETRAHEDRAL) ? gd->kernel_lut3d_tetrahedral
    : (d->params.interpolation == DT_IOP_TRILINEAR) ? gd->kernel_lut3d_trilinear
    : gd->kernel_lut3d_pyramid;
  const int colorspace
    = (d->params.colorspace == DT_IOP_SRGB) ? DT_COLORSPACE_SRGB
    : (d->params.colorspace == DT_IOP_REC709) ? DT_COLORSPACE_REC709
    : (d->params.colorspace == DT_IOP_ARGB) ? DT_COLORSPACE_ADOBERGB
    : (d->params.colorspace == DT_IOP_LIN_REC709) ? DT_COLORSPACE_LIN_REC709
    : (d->params.colorspace == DT_IOP_ITUR_BT1886) ? DT_COLORSPACE_ITUR_BT1886
    : DT_COLORSPACE_LIN_REC2020;
  const dt_iop_order_iccprofile_info_t *const lut_profile
    = dt_colorspaces_add_profile(colorspace, "", INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const work_profile
    = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  gboolean transform = (!IS_NULL_PTR(work_profile) && !IS_NULL_PTR(lut_profile)) ? TRUE : FALSE;
  cl_mem clut_cl = NULL;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  if (clut && level)
  {
    clut_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * level * level * level, (void *)clut);
    if(IS_NULL_PTR(clut_cl))
    {
      fprintf(stderr, "[lut3d process_cl] error allocating memory\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    if (transform)
    {
      const int success = dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_in, dev_out, width, height,
        work_profile, lut_profile, "work profile to LUT profile");
      if (!success)
       transform = FALSE;
    }
    if (transform)
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_out);
    else
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&clut_cl);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), (void *)&level);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if (transform)
      dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_out, dev_out, width, height,
        lut_profile, work_profile, "LUT profile to work profile");
  }
  else
  { // no lut: identity kernel
    dt_opencl_set_kernel_arg(devid, gd->kernel_lut3d_none, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lut3d_none, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lut3d_none, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lut3d_none, 3, sizeof(int), (void *)&height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lut3d_none, sizes);
  }
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "[lut3d process_cl] error %i enqueue kernel\n", err);
    goto cleanup;
  }

cleanup:
  dt_opencl_release_mem_object(clut_cl);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_lut3d] couldn't enqueue kernel! %d\n", err);
  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const ibuf, void *const obuf)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->dsc_in.channels;
  const float *const clut = (float *)d->clut;
  const uint16_t level = d->level;
  const int interpolation = d->params.interpolation;
  const int colorspace
    = (d->params.colorspace == DT_IOP_SRGB) ? DT_COLORSPACE_SRGB
    : (d->params.colorspace == DT_IOP_REC709) ? DT_COLORSPACE_REC709
    : (d->params.colorspace == DT_IOP_ARGB) ? DT_COLORSPACE_ADOBERGB
    : (d->params.colorspace == DT_IOP_LIN_REC709) ? DT_COLORSPACE_LIN_REC709
    : (d->params.colorspace == DT_IOP_ITUR_BT1886) ? DT_COLORSPACE_ITUR_BT1886
    : DT_COLORSPACE_LIN_REC2020;
  const dt_iop_order_iccprofile_info_t *const lut_profile
    = dt_colorspaces_add_profile(colorspace, "", INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const work_profile
    = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const gboolean transform = (!IS_NULL_PTR(work_profile) && !IS_NULL_PTR(lut_profile)) ? TRUE : FALSE;
  if (!IS_NULL_PTR(clut))
  {
    if (transform)
    {
      dt_ioppr_transform_image_colorspace_rgb(ibuf, obuf, width, height,
        work_profile, lut_profile, "work profile to LUT profile");
      dt_lut3d_apply(obuf, obuf, (size_t)width * height, clut, level, 1.f,
                     (dt_lut3d_interpolation_t)interpolation);
      dt_ioppr_transform_image_colorspace_rgb(obuf, obuf, width, height,
        lut_profile, work_profile, "LUT profile to work profile");
    }
    else
    {
      dt_lut3d_apply(ibuf, obuf, (size_t)width * height, clut, level, 1.f,
                     (dt_lut3d_interpolation_t)interpolation);
    }
  }
  else  // no clut
  {
    dt_iop_image_copy_by_size(obuf, ibuf, width, height, ch);
  }
  return 0;
}

void filepath_set_unix_separator(char *filepath)
{ // use the unix separator as it works also on windows
  const int len = strlen(filepath);
  for(int i=0; i<len; ++i)
    if (filepath[i]=='\\') filepath[i] = '/';
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 28; // rgbcurve.cl, from programs.conf
  dt_iop_lut3d_global_data_t *gd
      = (dt_iop_lut3d_global_data_t *)malloc(sizeof(dt_iop_lut3d_global_data_t));
  module->data = gd;
  gd->kernel_lut3d_tetrahedral = dt_opencl_create_kernel(program, "lut3d_tetrahedral");
  gd->kernel_lut3d_trilinear = dt_opencl_create_kernel(program, "lut3d_trilinear");
  gd->kernel_lut3d_pyramid = dt_opencl_create_kernel(program, "lut3d_pyramid");
  gd->kernel_lut3d_none = dt_opencl_create_kernel(program, "lut3d_none");

}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_lut3d_global_data_t *gd = (dt_iop_lut3d_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_lut3d_tetrahedral);
  dt_opencl_free_kernel(gd->kernel_lut3d_trilinear);
  dt_opencl_free_kernel(gd->kernel_lut3d_pyramid);
  dt_opencl_free_kernel(gd->kernel_lut3d_none);
  dt_free(module->data);
}

/* --- Deprecated: G'MIC compressed LUTs (.gmz) ------------------------------------------------
 *
 * Reading a .gmz meant linking the whole G'MIC library for three functions, and G'MIC brings a
 * 7.4 MB shared object plus ~50 transitive ones -- among them libcurl, OpenSSL, Kerberos and an
 * X11 display stack -- into a photo editor. Decompressing the format is not a matter of parsing
 * it either: the keypoints it stores are reconstructed either by a dense RBF solve or by a
 * multiscale diffusion PDE, both implemented in G'MIC's own scripting language. That is a lot of
 * machinery, and a lot of attack surface, for one LUT container.
 *
 * So .gmz is gone, and G'MIC with it. Every collection that ships .gmz also ships .cube, and
 * G'MIC's own CLI converts one to the other in a single command; the module documentation spells
 * out the procedure. See DT_LUT3D_GMZ_DOC_URL.
 *
 * Two things mark an edit as affected, and both must be caught: a filepath still pointing at a
 * .gmz, and -- for edits made before the removal -- keypoints serialised straight into the
 * history by an older version, which is what nb_keypoints > 0 means. The params keep those fields
 * so that existing histories still deserialise; they are simply never read any more.
 */
#define DT_LUT3D_GMZ_DOC_URL "https://ansel.photos/en/doc/views/darkroom/modules/lut-3d/"

static gboolean _params_need_gmz(const dt_iop_lut3d_params_t *const p)
{
  if(p->nb_keypoints > 0) return TRUE;
  return p->filepath[0] && (g_str_has_suffix(p->filepath, ".gmz") || g_str_has_suffix(p->filepath, ".GMZ"));
}

static int calculate_clut(dt_iop_lut3d_params_t *const p, float **clut)
{
  uint16_t level = 0;
  const char *filepath = p->filepath;
    gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
    if (filepath[0] && lutfolder[0])
    {
      char *fullpath = g_build_filename(lutfolder, filepath, NULL);
      if (g_str_has_suffix (filepath, ".png") || g_str_has_suffix (filepath, ".PNG"))
      {
        level = calculate_clut_haldclut(p, fullpath, clut);
      }
      else if (g_str_has_suffix (filepath, ".cube") || g_str_has_suffix (filepath, ".CUBE"))
      {
        level = calculate_clut_cube(fullpath, clut);
      }
      else if (g_str_has_suffix (filepath, ".3dl") || g_str_has_suffix (filepath, ".3DL"))
      {
        level = calculate_clut_3dl(fullpath, clut);
      }
      dt_free(fullpath);
    }
    dt_free(lutfolder);
  return level;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)p1;
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;

  if (strcmp(p->filepath, d->params.filepath) != 0 || strcmp(p->lutname, d->params.lutname) != 0 )
  { // new clut file
    if (d->clut)
    { // reset current clut if any
      dt_pixelpipe_cache_free_align(d->clut);
      d->clut = NULL;
      d->level = 0;
    }

    if(_params_need_gmz(p))
    {
      // Reached from every pipe -- darkroom, thumbnails, export -- and it is deliberately the
      // only notice outside the darkroom: one modal per thumbnail over a lighttable grid would
      // be unusable, so there the image has to be named in the log instead. Guarded by the
      // filepath comparison above, so it is printed when the LUT changes, not once per frame.
      fprintf(stderr,
              "[lut3d] %s: compressed G'MIC LUTs (.gmz) are no longer supported, so no LUT was "
              "applied to this image. Convert it to .cube -- see %s\n",
              (pipe && pipe->dev) ? pipe->dev->image_storage.filename : "(unknown image)",
              DT_LUT3D_GMZ_DOC_URL);
      d->level = 0;
    }
    else
      d->level = calculate_clut(p, &d->clut);
  }
  memcpy(&d->params, p, sizeof(dt_iop_lut3d_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_lut3d_data_t));
  piece->data_size = sizeof(dt_iop_lut3d_data_t);
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  memcpy(&d->params, self->default_params, sizeof(dt_iop_lut3d_params_t));
  d->clut = NULL;
  d->level = 0;
  d->params.filepath[0] = '\0';
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  /* init_pipe() may have failed to allocate, and cleanup runs regardless. */
  if(IS_NULL_PTR(piece->data)) return;
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;;
    dt_pixelpipe_cache_free_align(d->clut);
  d->clut = NULL;
  d->level = 0;
  dt_free_align(piece->data);
  piece->data = NULL;
}

static void filepath_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  char filepath[DT_IOP_LUT3D_MAX_PATHNAME];
  g_strlcpy(filepath, dt_bauhaus_combobox_get_text(widget), sizeof(filepath));
  fprintf(stdout, "filepath: %s\n", filepath);

  if (!g_str_has_prefix(filepath, invalid_filepath_prefix))
  {
    filepath_set_unix_separator(filepath);
    g_strlcpy(p->filepath, filepath, sizeof(p->filepath));
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  }
}


// remove root lut folder from path
static void remove_root_from_path(const char *const lutfolder, char *const filepath)
{
  const int j = strlen(lutfolder) + 1;
  int i;
  for(i = 0; filepath[i+j] != '\0'; i++)
    filepath[i] = filepath[i+j];
  filepath[i] = '\0';
}

gboolean check_extension(char *filename)
{
  gboolean res = FALSE;
  if (!filename || !filename[0]) return res;
  char *p = g_strrstr(filename,".");
  if (IS_NULL_PTR(p)) return res;
  char *fext = g_ascii_strdown(g_strdup(p), -1);
  if (!g_strcmp0(fext, ".png") || !g_strcmp0(fext, ".cube") || !g_strcmp0(fext, ".3dl") ) res = TRUE;
  dt_free(fext);
  return res;
}

static gint array_str_cmp(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(((dt_bauhaus_combobox_entry_t *)a)->label, ((dt_bauhaus_combobox_entry_t *)b)->label);
}

// update filepath combobox with all files in the current folder
static void update_filepath_combobox(dt_iop_lut3d_gui_data_t *g, char *filepath, char *lutfolder)
{
  if (!filepath[0])
    dt_bauhaus_combobox_clear(g->filepath);
  else if (!dt_bauhaus_combobox_set_from_text(g->filepath, filepath))
  {
    // new folder -> update the files list
    char *relativepath = g_path_get_dirname(filepath);
    char *folder = g_build_filename(lutfolder, relativepath, NULL);
    struct dirent *dir;
    DIR *d = opendir(folder);
    if(!IS_NULL_PTR(d))
    {
      dt_bauhaus_combobox_clear(g->filepath);
      while ((dir = readdir(d)) != NULL)
      {
        char *file = dir->d_name;
        if (check_extension(file))
        {
          char *ofilepath = (strcmp(relativepath, ".") != 0)
                ? g_build_filename(relativepath, file, NULL)
                : g_strdup(file);
          filepath_set_unix_separator(ofilepath);
          dt_bauhaus_combobox_add(g->filepath, ofilepath);
          dt_free(ofilepath);
        }
      }
      dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g->filepath);
      dt_bauhaus_combobox_data_t *combo_data = &w->data.combobox;
      g_ptr_array_sort(combo_data->entries, array_str_cmp);
      closedir(d);
    }
    if(!dt_bauhaus_combobox_set_from_text(g->filepath, filepath))
    { // file may have disappeared - show it
      char *invalidfilepath = g_strconcat(invalid_filepath_prefix, filepath, NULL);
      dt_bauhaus_combobox_add(g->filepath, invalidfilepath);
      dt_bauhaus_combobox_set_from_text(g->filepath, invalidfilepath);
      dt_free(invalidfilepath);
    }
    dt_free(relativepath);
    dt_free(folder);
  }
}

static void button_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  gchar* lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if (strlen(lutfolder) == 0)
  {
    fprintf(stderr, "[lut3d] Lut root folder not defined\n");
    dt_control_log(_("lut root folder not defined"));
    dt_free(lutfolder);
    return;
  }
  GtkWidget *win = dt_gui_main_window();
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("select lut file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
        _("_select"), _("_cancel"));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  char *composed = g_build_filename(lutfolder, p->filepath, NULL);
  if (strlen(p->filepath) == 0 || g_access(composed, F_OK) == -1)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), lutfolder);
  else
    gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(filechooser), composed);
  dt_free(composed);

  GtkFileFilter* filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.png");
  gtk_file_filter_add_pattern(filter, "*.PNG");
  gtk_file_filter_add_pattern(filter, "*.cube");
  gtk_file_filter_add_pattern(filter, "*.CUBE");
  gtk_file_filter_add_pattern(filter, "*.3dl");
  gtk_file_filter_add_pattern(filter, "*.3DL");
  gtk_file_filter_set_name(filter, _("hald cluts (png) or 3D lut (cube or 3dl)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(filechooser), filter);

  // let this option to allow the user to see the actual content of the folder
  // but any selected file with ext <> png or cube will be ignored
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    if (strcmp(lutfolder, filepath) < 0)
    {
      remove_root_from_path(lutfolder, filepath);
      filepath_set_unix_separator(filepath);
      update_filepath_combobox(g, filepath, lutfolder);
    }
    else if (!filepath[0])// file chosen outside of root folder
    {
      fprintf(stderr, "[lut3d] select file outside Lut root folder is not allowed\n");
      dt_control_log(_("select file outside Lut root folder is not allowed"));
    }
    dt_free(filepath);
    gtk_widget_set_sensitive(g->filepath, p->filepath[0]);
    g_strlcpy(p->filepath, dt_bauhaus_combobox_get_text(g->filepath), sizeof(p->filepath));
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  }
  dt_free(lutfolder);
  g_object_unref(filechooser);
}

static void _show_hide_colorspace(dt_iop_module_t *self)
{
  if(IS_NULL_PTR(self)) return;
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)dt_iop_gui_data(self);
  if(IS_NULL_PTR(g) || IS_NULL_PTR(g->colorspace)) return;
  GList *iop_order_list = self->dev->iop_order_list;
  const int order_lut3d = dt_ioppr_get_iop_order(iop_order_list, self->op, self->multi_priority);
  const int order_colorin = dt_ioppr_get_iop_order(iop_order_list, "colorin", -1);
  const int order_colorout = dt_ioppr_get_iop_order(iop_order_list, "colorout", -1);
  if(order_lut3d < order_colorin || order_lut3d > order_colorout)
  {
    gtk_widget_hide(g->colorspace);
  }
  else
  {
    gtk_widget_show(g->colorspace);
  }
}

/* Raised only from gui_update(), which means only in the darkroom: module GUIs are not
 * instantiated for lighttable thumbnails or for export, so there is no path by which this can
 * fire once per image over a grid. Outside the darkroom the notice is the log line in
 * commit_params(), which names the image instead. */
static void _warn_gmz_deprecated(const dt_iop_lut3d_params_t *const p)
{
  GtkWidget *win = dt_gui_main_window();
  if(IS_NULL_PTR(win)) return;

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE,
      _("This edit uses a compressed G'MIC LUT (.gmz), which Ansel no longer supports.\n\n"
        "No LUT is being applied to this image, so it will not look as it did. Convert the LUT "
        "to the .cube format and select it again -- the documentation gives the command.\n\n"
        "%s"),
      p->filepath[0] ? p->filepath : _("(the LUT was stored inside this edit)"));
  gtk_window_set_title(GTK_WINDOW(dialog), _("3D LUT: unsupported file format"));

  gtk_dialog_add_button(GTK_DIALOG(dialog), _("open documentation"), GTK_RESPONSE_HELP);
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_HELP)
    gtk_show_uri_on_window(GTK_WINDOW(win), DT_LUT3D_GMZ_DOC_URL, GDK_CURRENT_TIME, NULL);

  GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
  gtk_widget_destroy(dialog);
  dt_gui_refocus_parent(parent);
}

void gui_update(dt_iop_module_t *self)
{
  if(IS_NULL_PTR(self)) return;
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)dt_iop_gui_data(self);
    if(IS_NULL_PTR(g)) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
    if(IS_NULL_PTR(p)) return;
  gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if (!lutfolder[0])
  {
    gtk_widget_set_sensitive(g->hbox, FALSE);
    gtk_widget_set_sensitive(g->filepath, FALSE);
    dt_bauhaus_combobox_clear(g->filepath);
  }
  else
  {
    gtk_widget_set_sensitive(g->hbox, TRUE);
    gtk_widget_set_sensitive(g->filepath, p->filepath[0]);
    update_filepath_combobox(g, p->filepath, lutfolder);
  }
  dt_free(lutfolder);

  _show_hide_colorspace(self);

  if(_params_need_gmz(p) && (!g->gmz_warned || strcmp(g->gmz_warned_for, p->filepath) != 0))
  {
    g->gmz_warned = TRUE;
    g_strlcpy(g->gmz_warned_for, p->filepath, sizeof(g->gmz_warned_for));
    _warn_gmz_deprecated(p);
  }
}

void module_moved_callback(gpointer instance, dt_iop_module_t *self)
{
  _show_hide_colorspace(self);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = IOP_GUI_ALLOC(lut3d);

  self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  g->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_NONE, NULL);
  gtk_widget_set_tooltip_text(button, _("select a png (haldclut)"
      ", a cube or a 3dl file "
      "CAUTION: 3D lut folder must be set in preferences/processing before choosing the lut file"));
  gtk_box_pack_start(GTK_BOX(g->hbox), button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), self);

  g->filepath = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_combobox_set_entries_ellipsis(g->filepath, PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(g->hbox), g->filepath, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->filepath,
    _("the file path (relative to lut folder) is saved with image (and not the lut data themselves)"));
  g_signal_connect(G_OBJECT(g->filepath), "value-changed", G_CALLBACK(filepath_callback), self);

  gtk_box_pack_start(GTK_BOX(self->gui->widget), GTK_WIDGET(g->hbox), TRUE, TRUE, 0);


  g->colorspace = dt_bauhaus_combobox_from_params(self, "colorspace");
  gtk_widget_set_tooltip_text(g->colorspace, _("select the color space in which the LUT has to be applied"));

  g->interpolation = dt_bauhaus_combobox_from_params(self, N_("interpolation"));
  gtk_widget_set_tooltip_text(g->interpolation, _("select the interpolation method"));

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_MODULE_MOVED,
                            G_CALLBACK(module_moved_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(module_moved_callback), self);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
