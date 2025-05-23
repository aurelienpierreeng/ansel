/*
 * This file is part of ansel,
 * Copyright (C) 2019-2021 darktable developers.
 * Copyright (C) 2023 ansel developers.
 *
 *  Copyright (c) 2019      Andreas Schneider
 *
 *  ansel is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  ansel is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"
#include "control/control.h"
#include "imageio/format/imageio_format_api.h"

#include <avif/avif.h>

#define AVIF_MIN_TILE_SIZE 512
#define AVIF_MAX_TILE_SIZE 3072
#define AVIF_DEFAULT_TILE_SIZE AVIF_MIN_TILE_SIZE * 2

DT_MODULE(1)

enum avif_compression_type_e
{
  AVIF_COMP_LOSSLESS = 0,
  AVIF_COMP_LOSSY = 1,
};

enum avif_tiling_e
{
  AVIF_TILING_ON = 0,
  AVIF_TILING_OFF
};

enum avif_color_mode_e
{
  AVIF_COLOR_MODE_RGB = 0,
  AVIF_COLOR_MODE_GRAYSCALE,
};

typedef struct dt_imageio_avif_t
{
  dt_imageio_module_data_t global;
  uint32_t bit_depth;
  uint32_t color_mode;
  uint32_t compression_type;
  uint32_t quality;
  uint32_t tiling;
} dt_imageio_avif_t;

typedef struct dt_imageio_avif_gui_t
{
  GtkWidget *bit_depth;
  GtkWidget *color_mode;
  GtkWidget *compression_type;
  GtkWidget *quality;
  GtkWidget *tiling;
} dt_imageio_avif_gui_t;

static const struct
{
  char     *name;
  uint32_t bit_depth;
} avif_bit_depth[] = {
  {
    .name = N_("8 bit"),
    .bit_depth  = 8
  },
  {
    .name = N_("10 bit"),
    .bit_depth  = 10
  },
  {
    .name = N_("12 bit"),
    .bit_depth  = 12
  },
  {
    .name = NULL,
  }
};

static const char *avif_get_compression_string(enum avif_compression_type_e comp)
{
  switch(comp)
  {
    case AVIF_COMP_LOSSLESS:
      return "lossless";
    case AVIF_COMP_LOSSY:
      return "lossy";
  }

  return "unknown";
}

/* Lookup table for tiling choices */
static int floor_log2(int i)
{
  static const int floor_log2_table[] =
    /* 0   1,  2,  3,  4,  5,  6,  7,  8,  9 */
    {  0,  0,  2,  2,  4,  4,  4,  4,  8,  8,
       8,  8,  8,  8,  8,  8, 16, 16, 16, 16,
      16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
      16, 16, 32, 32, 32, 32, 32, 32, 32, 32,
      32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
      32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
      32, 32, 32, 32 };
    /* 0   1,  2,  3,  4,  5,  6,  7,  8,  9 */

  if(i >= 64)
  {
    return 64;
  }

  return floor_log2_table[i];
}

void init(dt_imageio_module_format_t *self)
{
  const char *codecName = avifCodecName(AVIF_CODEC_CHOICE_AUTO,
                                        AVIF_CODEC_FLAG_CAN_ENCODE);
  if(codecName == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "libavif doesn't offer encoding support!\n");
    self->ready = FALSE;
    return;
  }

#ifdef USE_LUA
  /* bit depth */
  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_avif_t,
                                bit_depth,
                                int);
  luaA_enum(darktable.lua_state.state,
            enum avif_color_mode_e);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_color_mode_e,
                  AVIF_COLOR_MODE_RGB);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_color_mode_e,
                  AVIF_COLOR_MODE_GRAYSCALE);

  luaA_enum(darktable.lua_state.state,
            enum avif_tiling_e);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_tiling_e,
                  AVIF_TILING_ON);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_tiling_e,
                  AVIF_TILING_OFF);

  /* compression type */
  luaA_enum(darktable.lua_state.state,
            enum avif_compression_type_e);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_compression_type_e,
                  AVIF_COMP_LOSSLESS);
  luaA_enum_value(darktable.lua_state.state,
                  enum avif_compression_type_e,
                  AVIF_COMP_LOSSY);

  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_avif_t,
                                compression_type,
                                enum avif_compression_type_e);

  /* quality */
  dt_lua_register_module_member(darktable.lua_state.state,
                                self,
                                dt_imageio_avif_t,
                                quality,
                                int);
#endif
}

void cleanup(dt_imageio_module_format_t *self)
{
}

int write_image(struct dt_imageio_module_data_t *data,
                const char *filename,
                const void *in,
                dt_colorspaces_color_profile_type_t over_type,
                const char *over_filename,
                void *exif,
                int exif_len,
                int32_t imgid,
                int num,
                int total,
                struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  dt_imageio_avif_t *d = (dt_imageio_avif_t *)data;

  avifPixelFormat format = AVIF_PIXEL_FORMAT_NONE;
  avifImage *image = NULL;
  avifRGBImage rgb = { .format = AVIF_RGB_FORMAT_RGB, };
  avifEncoder *encoder = NULL;
  uint8_t *icc_profile_data = NULL;
  uint32_t icc_profile_len;
  avifResult result;
  int rc;

  const size_t width = d->global.width;
  const size_t height = d->global.height;
  const size_t bit_depth = d->bit_depth > 0 ? d->bit_depth : 0;
  enum avif_color_mode_e color_mode = d->color_mode;

  switch(color_mode)
  {
    case AVIF_COLOR_MODE_RGB:
      switch(d->compression_type)
      {
        case AVIF_COMP_LOSSLESS:
          format = AVIF_PIXEL_FORMAT_YUV444;
          break;
        case AVIF_COMP_LOSSY:
          if(d->quality > 90)
          {
              format = AVIF_PIXEL_FORMAT_YUV444;
          }
          else if(d->quality > 80)
          {
              format = AVIF_PIXEL_FORMAT_YUV422;
          }
          else
          {
            format = AVIF_PIXEL_FORMAT_YUV420;
          }
          break;
      }

      break;
    case AVIF_COLOR_MODE_GRAYSCALE:
      format = AVIF_PIXEL_FORMAT_YUV400;
      break;
  }

  image = avifImageCreate(width, height, bit_depth, format);
  if(image == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to create AVIF image for writing [%s]\n",
             filename);
    rc = 1;
    goto out;
  }

  dt_print(DT_DEBUG_IMAGEIO,
           "Exporting AVIF image [%s] "
           "[width: %zu, height: %zu, bit depth: %zu, comp: %s, quality: %u]\n",
           filename,
           width,
           height,
           bit_depth,
           avif_get_compression_string(d->compression_type),
           d->quality);

  if(imgid > 0)
  {
    gboolean use_icc = FALSE;

    /*
     * Set these in advance so any upcoming RGB -> YUV use the proper
     * coefficients.
     */
    switch(over_type)
    {
      case DT_COLORSPACE_SRGB:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
          break;
      case DT_COLORSPACE_REC709:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_BT709;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
        break;
      case DT_COLORSPACE_LIN_REC709:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT709;
        break;
      case DT_COLORSPACE_LIN_REC2020:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
        break;
      case DT_COLORSPACE_PQ_REC2020:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
        break;
      case DT_COLORSPACE_HLG_REC2020:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_HLG;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
        break;
      case DT_COLORSPACE_PQ_P3:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_SMPTE432;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL;
        break;
      case DT_COLORSPACE_HLG_P3:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_SMPTE432;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_HLG;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL;
        break;
      case DT_COLORSPACE_DISPLAY_P3:
          image->colorPrimaries = AVIF_COLOR_PRIMARIES_SMPTE432;
          image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
          image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL;
        break;
      default:
        break;
    }

    // no change from default, unspecified CICP (2/2/2)
    if(image->colorPrimaries == AVIF_COLOR_PRIMARIES_UNSPECIFIED)
    {
      use_icc = TRUE;
    }

    dt_print(DT_DEBUG_IMAGEIO, "[avif colorprofile profile: %s - %s]\n",
             dt_colorspaces_get_name(over_type, filename),
             use_icc ? "icc" : "nclx");

    if(use_icc)
    {
      const dt_colorspaces_color_profile_t *cp =
        dt_colorspaces_get_output_profile(imgid,
                                          &over_type,
                                          over_filename);
      cmsHPROFILE out_profile = cp->profile;

      cmsSaveProfileToMem(out_profile, 0, &icc_profile_len);
      if(icc_profile_len > 0)
      {
        icc_profile_data = malloc(sizeof(uint8_t) * icc_profile_len);
        if(icc_profile_data == NULL)
        {
          rc = 1;
          goto out;
        }
        cmsSaveProfileToMem(out_profile, icc_profile_data, &icc_profile_len);
        (void)avifImageSetProfileICC(image, icc_profile_data, icc_profile_len);
      }
    }
  }

  /*
   * Set the YUV range before conversion.
   *
   * Limited range (aka "studio range", "studio swing", etc) is simply when you
   * cut off the ends of the actual range you have to avoid the actual minimum
   * and maximum of the signal. For example, instead of having full range 8bpc
   * ([0-255]) in each channel, you'd only use [16-235]. Anything 16 or below
   * is treated as a 0.0 signal, and anything 235 or higher is treated as a 1.0
   * signal.
   *
   * The *reason* this exists, is largely vestigial from the analog era.
   *
   * For picture we always want the full range.
   */
  image->yuvRange = AVIF_RANGE_FULL;

  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGB;

  (void)avifRGBImageAllocatePixels(&rgb);

  const float max_channel_f = (float)((1 << bit_depth) - 1);

  const size_t rowbytes = rgb.rowBytes;

  const float *const restrict in_data = (const float *)in;
  uint8_t *const restrict out = (uint8_t *)rgb.pixels;

  switch(bit_depth)
  {
    case 12:
    case 10:
    {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in_data, width, height, out, rowbytes, max_channel_f) \
  schedule(simd:static) \
  collapse(2)
#endif
    for(size_t y = 0; y < height; y++)
    {
      for(size_t x = 0; x < width; x++)
      {
          const float *in_pixel = &in_data[(size_t)4 * ((y * width) + x)];
          uint16_t *out_pixel = (uint16_t *)&out[(y * rowbytes) + (3 * sizeof(uint16_t) * x)];

          out_pixel[0] = (uint16_t)roundf(CLAMP(in_pixel[0] * max_channel_f, 0, max_channel_f));
          out_pixel[1] = (uint16_t)roundf(CLAMP(in_pixel[1] * max_channel_f, 0, max_channel_f));
          out_pixel[2] = (uint16_t)roundf(CLAMP(in_pixel[2] * max_channel_f, 0, max_channel_f));
      }
    }
    break;
    }
    case 8:
    {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in_data, width, height, out, rowbytes, max_channel_f) \
  schedule(simd:static) \
  collapse(2)
#endif
    for(size_t y = 0; y < height; y++)
    {
      for(size_t x = 0; x < width; x++)
      {
          const float *in_pixel = &in_data[(size_t)4 * ((y * width) + x)];
          uint8_t *out_pixel = (uint8_t *)&out[(y * rowbytes) + (3 * sizeof(uint8_t) * x)];

          out_pixel[0] = (uint8_t)roundf(CLAMP(in_pixel[0] * max_channel_f, 0, max_channel_f));
          out_pixel[1] = (uint8_t)roundf(CLAMP(in_pixel[1] * max_channel_f, 0, max_channel_f));
          out_pixel[2] = (uint8_t)roundf(CLAMP(in_pixel[2] * max_channel_f, 0, max_channel_f));
      }
    }
    break;
    }
    default:
      dt_control_log(_("invalid AVIF bit depth!"));
      rc = 1;
      goto out;
  }

  (void)avifImageRGBToYUV(image, &rgb);

  if(exif && exif_len > 0)
    (void)avifImageSetMetadataExif(image, exif, exif_len);

  /* TODO: workaround; remove when exiv2 implements AVIF write support and update flags() */
  char *xmp_string = dt_exif_xmp_read_string(imgid);
  size_t xmp_len;
  if(xmp_string && (xmp_len = strlen(xmp_string)) > 0)
  {
    (void)avifImageSetMetadataXMP(image, (const uint8_t *)xmp_string, xmp_len);
    g_free(xmp_string);
  }

  encoder = avifEncoderCreate();
  if(encoder == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to create AVIF encoder for image [%s]\n",
             filename);
    rc = 1;
    goto out;
  }

  switch(d->compression_type)
  {
    case AVIF_COMP_LOSSLESS:
      /* It isn't recommend to use the extremities */
      encoder->speed = AVIF_SPEED_SLOWEST + 1;

      encoder->minQuantizer = AVIF_QUANTIZER_LOSSLESS;
      encoder->maxQuantizer = AVIF_QUANTIZER_LOSSLESS;

      break;
    case AVIF_COMP_LOSSY:
      encoder->speed = AVIF_SPEED_DEFAULT;

      encoder->maxQuantizer = 100 - d->quality;
      encoder->maxQuantizer = CLAMP(encoder->maxQuantizer, 0, 63);

      encoder->minQuantizer = 64 - d->quality;
      encoder->minQuantizer = CLAMP(encoder->minQuantizer, 0, 63);
      break;
  }

  /*
   * Tiling reduces the image quality but it has a negligible impact on
   * still images.
   *
   * The minimum size for a tile is 512x512. We use a default tile size of
   * 1024x1024.
   */
  switch(d->tiling)
  {
    case AVIF_TILING_ON:
    {
      size_t width_tile_size  = AVIF_DEFAULT_TILE_SIZE;
      size_t height_tile_size = AVIF_DEFAULT_TILE_SIZE;
      size_t max_threads;

      if(width >= 6144)
      {
        width_tile_size = AVIF_MIN_TILE_SIZE * 4;
      }
      else if (width >= 8192) {
        width_tile_size = AVIF_MAX_TILE_SIZE;
      }
      if(height >= 6144)
      {
        height_tile_size = AVIF_MIN_TILE_SIZE * 4;
      }
      else if (height >= 8192) {
        height_tile_size = AVIF_MAX_TILE_SIZE;
      }

      encoder->tileColsLog2 = floor_log2(width / width_tile_size) / 2;
      encoder->tileRowsLog2 = floor_log2(height / height_tile_size) / 2;

      /*
       * This should be set to the final number of tiles, based on
       * encoder->tileColsLog2 and encoder->tileRowsLog2.
       */
      max_threads = (1 << encoder->tileRowsLog2) * (1 << encoder->tileColsLog2);

      encoder->maxThreads = MIN(max_threads, darktable.num_openmp_threads);
    }
    case AVIF_TILING_OFF:
      break;
  }

  dt_print(DT_DEBUG_IMAGEIO,
           "[avif quality: %u => maxQuantizer: %u, minQuantizer: %u, "
           "tileColsLog2: %u, tileRowsLog2: %u, threads: %u]\n",
           d->quality,
           encoder->maxQuantizer,
           encoder->minQuantizer,
           encoder->tileColsLog2,
           encoder->tileRowsLog2,
           encoder->maxThreads);

  avifRWData output = AVIF_DATA_EMPTY;

  result = avifEncoderWrite(encoder, image, &output);
  if(result != AVIF_RESULT_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "Failed to encode AVIF image [%s]: %s\n",
             filename, avifResultToString(result));
    rc = 1;
    goto out;
  }

  if(output.size == 0 || output.data == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO,
             "AVIF encoder returned empty data for [%s]\n",
             filename);
    rc = 1;
    goto out;
  }

  /*
   * Write image to disk
   */
  FILE *f = NULL;
  size_t cnt = 0;

  f = g_fopen(filename, "wb");
  if(f == NULL)
  {
    rc = 1;
    goto out;
  }

  cnt = fwrite(output.data, 1, output.size, f);
  fclose(f);
  if(cnt != output.size)
  {
    g_unlink(filename);
    rc = 1;
    goto out;
  }

  rc = 0; /* success */
out:
  avifRGBImageFreePixels(&rgb);
  avifImageDestroy(image);
  avifEncoderDestroy(encoder);
  avifRWDataFree(&output);
  free(icc_profile_data);

  return rc;
}


size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_avif_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_avif_t *d = (dt_imageio_avif_t *)calloc(1, sizeof(dt_imageio_avif_t));

  if(d == NULL)
  {
    return NULL;
  }

  d->bit_depth = dt_conf_get_int("plugins/imageio/format/avif/bpp");
  if(d->bit_depth != 10 && d->bit_depth != 12)
    d->bit_depth = 8;

  d->color_mode = dt_conf_get_int("plugins/imageio/format/avif/color_mode");
  d->compression_type = dt_conf_get_int("plugins/imageio/format/avif/compression_type");

  switch(d->compression_type)
  {
    case AVIF_COMP_LOSSLESS:
      d->quality = 100;
      break;
    case AVIF_COMP_LOSSY:
      d->quality = dt_conf_get_int("plugins/imageio/format/avif/quality");
      if(d->quality > 100)
      {
        d->quality = 100;
      }
      break;
  }

  d->tiling = !dt_conf_get_bool("plugins/imageio/format/avif/tiling");

  return d;
}

int set_params(dt_imageio_module_format_t *self,
               const void *params,
               const int size)
{
  if(size != self->params_size(self))
    return 1;
  const dt_imageio_avif_t *d = (dt_imageio_avif_t *)params;

  dt_imageio_avif_gui_t *g = (dt_imageio_avif_gui_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->bit_depth, d->bit_depth);
  dt_bauhaus_combobox_set(g->color_mode, d->color_mode);
  dt_bauhaus_combobox_set(g->tiling, d->tiling);
  dt_bauhaus_combobox_set(g->compression_type, d->compression_type);
  dt_bauhaus_slider_set(g->quality, d->quality);

  return 0;
}

void free_params(dt_imageio_module_format_t *self,
                 dt_imageio_module_data_t *params)
{
  free(params);
}


int bpp(struct dt_imageio_module_data_t *data)
{
  return 32; /* always request float */
}

int levels(struct dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB|IMAGEIO_FLOAT;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "image/avif";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "avif";
}

const char *name()
{
  return _("AVIF (8/10/12-bit)");
}

int flags(struct dt_imageio_module_data_t *data)
{
  /*
   * As of exiv2 0.27.5 there is no write support for the AVIF format, so
   * we do not return the XMP supported flag currently.
   * Once exiv2 write support is there, the flag can be returned, and the
   * direct XMP embedding workaround using avifImageSetMetadataXMP() above
   * can be removed.
   */
  return 0; /* FORMAT_FLAGS_SUPPORT_XMP; */
}

static void bit_depth_changed(GtkWidget *widget, gpointer user_data)
{
  const uint32_t idx = dt_bauhaus_combobox_get(widget);

  dt_conf_set_int("plugins/imageio/format/avif/bpp", avif_bit_depth[idx].bit_depth);
}

static void color_mode_changed(GtkWidget *widget, gpointer user_data)
{
  const enum avif_color_mode_e color_mode = dt_bauhaus_combobox_get(widget);

  dt_conf_set_int("plugins/imageio/format/avif/color_mode", color_mode);
}

static void tiling_changed(GtkWidget *widget, gpointer user_data)
{
  const enum avif_tiling_e tiling = dt_bauhaus_combobox_get(widget);

  dt_conf_set_bool("plugins/imageio/format/avif/tiling", !tiling);
}

static void compression_type_changed(GtkWidget *widget, gpointer user_data)
{
  const enum avif_compression_type_e compression_type = dt_bauhaus_combobox_get(widget);
  dt_imageio_module_format_t *module = (dt_imageio_module_format_t *)user_data;
  dt_imageio_avif_gui_t *gui = (dt_imageio_avif_gui_t *)module->gui_data;

  dt_conf_set_int("plugins/imageio/format/avif/compression_type", compression_type);

  switch(compression_type)
  {
    case AVIF_COMP_LOSSLESS:
      gtk_widget_set_sensitive(gui->quality, FALSE);
      break;
    case AVIF_COMP_LOSSY:
      gtk_widget_set_sensitive(gui->quality, TRUE);
      break;
  }
}

static void quality_changed(GtkWidget *slider, gpointer user_data)
{
  const uint32_t quality = (int)dt_bauhaus_slider_get(slider);
  dt_conf_set_int("plugins/imageio/format/avif/quality", quality);
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_avif_gui_t *gui =
      (dt_imageio_avif_gui_t *)malloc(sizeof(dt_imageio_avif_gui_t));
  const uint32_t bit_depth = dt_conf_get_int("plugins/imageio/format/avif/bit_depth");
  const enum avif_color_mode_e color_mode = dt_conf_get_int("plugins/imageio/format/avif/color_mode");
  const enum avif_tiling_e tiling = !dt_conf_get_bool("plugins/imageio/format/avif/tiling");
  const enum avif_compression_type_e compression_type = dt_conf_get_int("plugins/imageio/format/avif/compression_type");
  const uint32_t quality = dt_conf_get_int("plugins/imageio/format/avif/quality");

  self->gui_data = (void *)gui;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /*
   * Bit depth combo box
   */
  gui->bit_depth = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));

  dt_bauhaus_widget_set_label(gui->bit_depth, N_("bit depth"));
  size_t idx = 0;
  for(size_t i = 0; avif_bit_depth[i].name != NULL; i++)
  {
    dt_bauhaus_combobox_add(gui->bit_depth,  _(avif_bit_depth[i].name));
    if(avif_bit_depth[i].bit_depth == bit_depth)
    {
      idx = i;
    }
  }
  dt_bauhaus_combobox_set(gui->bit_depth, idx);

  gtk_widget_set_tooltip_text(gui->bit_depth,
          _("color information stored in an image, higher is better"));

  gtk_box_pack_start(GTK_BOX(self->widget), gui->bit_depth, TRUE, TRUE, 0);

  /*
   * Color mode combo box
   */
  gui->color_mode = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(gui->color_mode, _("color mode"));
  dt_bauhaus_combobox_add(gui->color_mode,
                          _("rgb colors"));
  dt_bauhaus_combobox_add(gui->color_mode,
                          _("grayscale"));
  dt_bauhaus_combobox_set(gui->color_mode, color_mode);

  gtk_widget_set_tooltip_text(gui->color_mode,
          _("saving as grayscale will reduce the size for black & white images"));

  gtk_box_pack_start(GTK_BOX(self->widget),
                     gui->color_mode,
                     TRUE,
                     TRUE,
                     0);
  /*
   * Tiling combo box
   */
  gui->tiling = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(gui->tiling, N_("tiling"));
  dt_bauhaus_combobox_add(gui->tiling,
                          _("on"));
  dt_bauhaus_combobox_add(gui->tiling,
                          _("off"));
  dt_bauhaus_combobox_set(gui->tiling, tiling);

  gtk_widget_set_tooltip_text(gui->tiling,
          _("tile an image into segments.\n"
            "\n"
            "makes encoding faster. the impact on quality reduction "
            "is negligible, but increases the file size."));

  gtk_box_pack_start(GTK_BOX(self->widget),
                     gui->tiling,
                     TRUE,
                     TRUE,
                     0);

  /*
   * Compression type combo box
   */
  gui->compression_type = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(gui->compression_type, N_("compression type"));
  dt_bauhaus_combobox_add(gui->compression_type,
                          _(avif_get_compression_string(AVIF_COMP_LOSSLESS)));
  dt_bauhaus_combobox_add(gui->compression_type,
                          _(avif_get_compression_string(AVIF_COMP_LOSSY)));
  dt_bauhaus_combobox_set(gui->compression_type, compression_type);

  gtk_widget_set_tooltip_text(gui->compression_type,
          _("the compression for the image"));

  gtk_box_pack_start(GTK_BOX(self->widget),
                     gui->compression_type,
                     TRUE,
                     TRUE,
                     0);

  /*
   * Quality combo box
   */
  gui->quality = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(NULL),
                                                  dt_confgen_get_int("plugins/imageio/format/avif/quality", DT_MIN), /* min */
                                                  dt_confgen_get_int("plugins/imageio/format/avif/quality", DT_MAX), /* max */
                                                  1, /* step */
                                                  dt_confgen_get_int("plugins/imageio/format/avif/quality", DT_DEFAULT), /* default */
                                                  0); /* digits */
  dt_bauhaus_widget_set_label(gui->quality, N_("quality"));
  dt_bauhaus_slider_set_default(gui->quality, dt_confgen_get_int("plugins/imageio/format/avif/quality", DT_DEFAULT));
  dt_bauhaus_slider_set_format(gui->quality, "%");

  gtk_widget_set_tooltip_text(gui->quality,
          _("the quality of an image, less quality means fewer details.\n"
            "\n"
            "the following applies only to lossy setting\n"
            "\n"
            "pixelformat based on quality:\n"
            "\n"
            "    91% - 100% -> YUV444\n"
            "    81% -  90% -> YUV422\n"
            "     5% -  80% -> YUV420\n"));

  if(quality > 0 && quality <= 100)
  {
      dt_bauhaus_slider_set(gui->quality, quality);
  }
  gtk_box_pack_start(GTK_BOX(self->widget), gui->quality, TRUE, TRUE, 0);

  switch(compression_type)
  {
    case AVIF_COMP_LOSSLESS:
      gtk_widget_set_sensitive(gui->quality, FALSE);
      break;
    case AVIF_COMP_LOSSY:
      break;
  }

  g_signal_connect(G_OBJECT(gui->bit_depth),
                   "value-changed",
                   G_CALLBACK(bit_depth_changed),
                   NULL);
  g_signal_connect(G_OBJECT(gui->color_mode),
                   "value-changed",
                   G_CALLBACK(color_mode_changed),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(gui->tiling),
                   "value-changed",
                   G_CALLBACK(tiling_changed),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(gui->compression_type),
                   "value-changed",
                   G_CALLBACK(compression_type_changed),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(gui->quality),
                   "value-changed",
                   G_CALLBACK(quality_changed),
                   NULL);
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_avif_gui_t *gui = (dt_imageio_avif_gui_t *)self->gui_data;

  const enum avif_color_mode_e color_mode = dt_confgen_get_int("plugins/imageio/format/avif/color_mode", DT_DEFAULT);
  const enum avif_tiling_e tiling = !dt_confgen_get_bool("plugins/imageio/format/avif/tiling", DT_DEFAULT);
  const enum avif_compression_type_e compression_type = dt_confgen_get_int("plugins/imageio/format/avif/compression_type", DT_DEFAULT);
  const uint32_t quality = dt_confgen_get_int("plugins/imageio/format/avif/quality", DT_DEFAULT);

  dt_bauhaus_combobox_set(gui->bit_depth, 0); //8bpp
  dt_bauhaus_combobox_set(gui->color_mode, color_mode);
  dt_bauhaus_combobox_set(gui->tiling, tiling);
  dt_bauhaus_combobox_set(gui->compression_type, compression_type);
  dt_bauhaus_slider_set(gui->quality, quality);

  compression_type_changed(GTK_WIDGET(gui->compression_type), self);
  quality_changed(GTK_WIDGET(gui->quality), self);
  bit_depth_changed(GTK_WIDGET(gui->bit_depth), self);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
