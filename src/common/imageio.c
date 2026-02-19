/*
    This file is part of darktable,
    Copyright (C) 2009-2014 johannes hanika.
    Copyright (C) 2010 Anton Blanchard.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010 Kaminsky Andrey.
    Copyright (C) 2010-2020 Tobias Ellinghaus.
    Copyright (C) 2011 Bruce Guenter.
    Copyright (C) 2011 Omari Stephens.
    Copyright (C) 2011 Sergey Pavlov.
    Copyright (C) 2011-2014, 2019 Ulrich Pegelow.
    Copyright (C) 2012-2015 Jérémy Rosen.
    Copyright (C) 2012-2014 Pascal de Bruijn.
    Copyright (C) 2012-2015, 2018-2021 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2014-2018 Roman Lebedev.
    Copyright (C) 2015 Edouard Gomez.
    Copyright (C) 2016 Chris Hodapp.
    Copyright (C) 2016, 2020 Matthieu Volat.
    Copyright (C) 2017 Žilvinas Žaltiena.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Sören Witt.
    Copyright (C) 2019, 2022 Aldric Renaudin.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019-2022 Hanno Schwalm.
    Copyright (C) 2019-2021 Heiko Bauke.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020 a.
    Copyright (C) 2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Miloš Komarčević.
    Copyright (C) 2021 Daniel Vogelbacher.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 parafin.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2023-2024 Alynx Zhou.
    Copyright (C) 2023 Ricky Moon.
    
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
#include "common/image.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/colorlabels.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#ifdef HAVE_OPENEXR
#include "common/imageio_exr.h"
#endif
#ifdef HAVE_OPENJPEG
#include "common/imageio_j2k.h"
#endif
#include "common/imageio_gm.h"
#include "common/imageio_im.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_pfm.h"
#include "common/imageio_png.h"
#include "common/imageio_pnm.h"
#include "common/imageio_rawspeed.h"
#include "common/imageio_libraw.h"
#include "common/imageio_rgbe.h"
#include "common/imageio_tiff.h"
#ifdef HAVE_LIBAVIF
#include "common/imageio_avif.h"
#endif
#ifdef HAVE_LIBHEIF
#include "common/imageio_heif.h"
#endif
#ifdef HAVE_WEBP
#include "common/imageio_webp.h"
#endif
#include "common/imageio_libraw.h"
#include "common/mipmap_cache.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#if defined(HAVE_GRAPHICSMAGICK)
#include <magick/api.h>
#include <magick/blob.h>
#elif defined(HAVE_IMAGEMAGICK)
#include <MagickWand/MagickWand.h>
#endif

#include <assert.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef USE_LUA
#include "lua/image.h"
#endif

// note `dng` is not included anywhere as it can be anything. For this images we'll need to open it for "real"
static const gchar *_supported_raw[]
    = { "3fr", "ari", "arw", "bay", "cr2", "cr3", "crw", "dc2", "dcr", "erf", "fff",
        "ia",  "iiq", "k25", "kc2", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw",
        "orf", "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw", "sti", "x3f", NULL };
static const gchar *_supported_ldr[]
    = { "bmp",  "bmq", "cap", "cine", "cs1", "dcm", "gif", "gpr", "j2c", "j2k", "jng", "jp2", "jpc", "jpeg", "jpg",
        "miff", "mng", "ori", "pbm",  "pfm", "pgm", "png", "pnm", "ppm", "pxn", "qtk", "rdc", "tif", "tiff", "webp",
        NULL };
static const gchar *_supported_hdr[] = { "avif", "exr", "hdr", "heic", "heif", "hif", "pfm", NULL };

// get the type of image from its extension
dt_image_flags_t dt_imageio_get_type_from_extension(const char *extension)
{
  const char *ext = g_str_has_prefix(extension, ".") ? extension + 1 : extension;
  for(const char **i = _supported_raw; *i != NULL; i++)
  {
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      return DT_IMAGE_RAW;
    }
  }
  for(const char **i = _supported_hdr; *i != NULL; i++)
  {
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      return DT_IMAGE_HDR;
    }
  }
  for(const char **i = _supported_ldr; *i != NULL; i++)
  {
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      return DT_IMAGE_LDR;
    }
  }
  // default to 0
  return 0;
}

int dt_imageio_large_thumbnail(const char *filename, uint8_t **buffer, int32_t *th_width, int32_t *th_height,
                               dt_colorspaces_color_profile_type_t *color_space, const int width, const int height)
{
  int res = 1;

  uint8_t *buf = NULL;
  char *mime_type = NULL;
  size_t bufsize;

  if(dt_exif_get_thumbnail(filename, &buf, &bufsize, &mime_type, th_width, th_height, MAX(width, height))) 
    goto error;

  if(strcmp(mime_type, "image/jpeg") == 0)
  {
    // Decompress the JPG into our own memory format
    dt_imageio_jpeg_t jpg;
    if(dt_imageio_jpeg_decompress_header(buf, bufsize, &jpg)) goto error;
    *buffer = (uint8_t *)dt_pixelpipe_cache_alloc_align_cache(
        sizeof(uint8_t) * 4 * jpg.width * jpg.height,
        0);
    if(!*buffer) goto error;

    *th_width = jpg.width;
    *th_height = jpg.height;
    // TODO: check if the embedded thumbs have a color space set! currently we assume that it's always sRGB
    *color_space = DT_COLORSPACE_SRGB;
    if(dt_imageio_jpeg_decompress(&jpg, *buffer))
    {
      dt_pixelpipe_cache_free_align(*buffer);
      *buffer = NULL;
      goto error;
    }

    res = 0;
  }
  else
  {
#if defined(HAVE_GRAPHICSMAGICK)
    ExceptionInfo exception;
    Image *image = NULL;
    ImageInfo *image_info = NULL;

    GetExceptionInfo(&exception);
    image_info = CloneImageInfo((ImageInfo *)NULL);

    image = BlobToImage(image_info, buf, bufsize, &exception);

    if(exception.severity != UndefinedException) CatchException(&exception);

    if(!image)
    {
      fprintf(stderr, "[dt_imageio_large_thumbnail GM] thumbnail not found?\n");
      goto error_gm;
    }

    *th_width = image->columns;
    *th_height = image->rows;
    *color_space = DT_COLORSPACE_SRGB; // FIXME: this assumes that embedded thumbnails are always srgb

    *buffer = (uint8_t *)dt_pixelpipe_cache_alloc_align_cache(
        sizeof(uint8_t) * 4 * image->columns * image->rows,
        0);
    if(!*buffer) goto error_gm;

    for(uint32_t row = 0; row < image->rows; row++)
    {
      uint8_t *bufprt = *buffer + (size_t)4 * row * image->columns;
      int gm_ret = DispatchImage(image, 0, row, image->columns, 1, "RGBP", CharPixel, bufprt, &exception);

      if(exception.severity != UndefinedException) CatchException(&exception);

      if(gm_ret != MagickPass)
      {
        fprintf(stderr, "[dt_imageio_large_thumbnail GM] error_gm reading thumbnail\n");
        dt_pixelpipe_cache_free_align(*buffer);
        *buffer = NULL;
        goto error_gm;
      }
    }

    // fprintf(stderr, "[dt_imageio_large_thumbnail GM] successfully decoded thumbnail\n");
    res = 0;

  error_gm:
    if(image) DestroyImage(image);
    if(image_info) DestroyImageInfo(image_info);
    DestroyExceptionInfo(&exception);
    if(res) goto error;
#elif defined(HAVE_IMAGEMAGICK)
    MagickWand *image = NULL;
	MagickBooleanType mret;

    image = NewMagickWand();
	mret = MagickReadImageBlob(image, buf, bufsize);
    if(mret != MagickTrue)
    {
      fprintf(stderr, "[dt_imageio_large_thumbnail IM] thumbnail not found?\n");
      goto error_im;
    }

    *th_width = MagickGetImageWidth(image);
    *th_height = MagickGetImageHeight(image);
    switch (MagickGetImageColorspace(image)) {
    case sRGBColorspace:
      *color_space = DT_COLORSPACE_SRGB;
      break;
    default:
      fprintf(stderr,
          "[dt_imageio_large_thumbnail IM] could not map colorspace, using sRGB");
      *color_space = DT_COLORSPACE_SRGB;
      break;
    }

    *buffer = malloc(sizeof(uint8_t) * (*th_width) * (*th_height) * 4);
    if(*buffer == NULL) goto error_im;

    mret = MagickExportImagePixels(image, 0, 0, *th_width, *th_height, "RGBP", CharPixel, *buffer);
    if(mret != MagickTrue) {
      free(*buffer);
      *buffer = NULL;
      fprintf(stderr,
          "[dt_imageio_large_thumbnail IM] error while reading thumbnail\n");
      goto error_im;
    }

    res = 0;

error_im:
    DestroyMagickWand(image);
    if(res != 0) goto error;
#else
    fprintf(stderr,
      "[dt_imageio_large_thumbnail] error: The thumbnail image is not in "
      "JPEG format, and DT was built without neither GraphicsMagick or "
      "ImageMagick. Please rebuild DT with GraphicsMagick or ImageMagick "
      "support enabled.\n");
#endif
  }

  if(res)
  {
    fprintf(
        stderr,
        "[dt_imageio_large_thumbnail] error: Not a supported thumbnail image format or broken thumbnail: %s\n",
        mime_type);
    goto error;
  }

error:
  free(mime_type);
  free(buf);
  return res;
}

gboolean dt_imageio_has_mono_preview(const char *filename)
{
  dt_colorspaces_color_profile_type_t color_space;
  uint8_t *tmp = NULL;
  int32_t thumb_width = 0, thumb_height = 0;
  gboolean mono = FALSE;

  if(dt_imageio_large_thumbnail(filename, &tmp, &thumb_width, &thumb_height, &color_space, -1, -1))
    goto cleanup;
  if((thumb_width < 32) || (thumb_height < 32) || (tmp == NULL))
    goto cleanup;

  mono = TRUE;
  for(int y = 0; y < thumb_height; y++)
  {
    uint8_t *in = (uint8_t *)tmp + (size_t)4 * y * thumb_width;
    for(int x = 0; x < thumb_width; x++, in += 4)
    {
      if((in[0] != in[1]) || (in[0] != in[2]) || (in[1] != in[2]))
      {
        mono = FALSE;
        goto cleanup;
      }
    }
  }

  cleanup:

  dt_print(DT_DEBUG_IMAGEIO,"[dt_imageio_has_mono_preview] testing `%s', yes/no %i, %ix%i\n", filename, mono, thumb_width, thumb_height);
  dt_pixelpipe_cache_free_align(tmp);
  return mono;
}

void dt_imageio_flip_buffers(char *out, const char *in, const size_t bpp, const int wd, const int ht,
                             const int fwd, const int fht, const int stride,
                             const dt_image_orientation_t orientation)
{
  if(!orientation)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ht, wd, bpp, stride) \
    shared(in, out) \
    schedule(static)
#endif
    for(int j = 0; j < ht; j++) memcpy(out + (size_t)j * bpp * wd, in + (size_t)j * stride, bpp * wd);
    return;
  }
  int ii = 0, jj = 0;
  int si = bpp, sj = wd * bpp;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    sj = bpp;
    si = ht * bpp;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(wd, bpp, ht, stride) \
  shared(in, out, jj, ii, sj, si) \
  schedule(static)
#endif
  for(int j = 0; j < ht; j++)
  {
    char *out2 = out + (size_t)labs(sj) * jj + (size_t)labs(si) * ii + (size_t)sj * j;
    const char *in2 = in + (size_t)stride * j;
    for(int i = 0; i < wd; i++)
    {
      memcpy(out2, in2, bpp);
      in2 += bpp;
      out2 += si;
    }
  }
}

void dt_imageio_flip_buffers_ui8_to_float(float *out, const uint8_t *in, const float black, const float white,
                                          const int ch, const int wd, const int ht, const int fwd,
                                          const int fht, const int stride,
                                          const dt_image_orientation_t orientation)
{
  const float scale = 1.0f / (white - black);
  if(!orientation)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(wd, scale, black, ht, ch, stride) \
    shared(in, out) \
    schedule(static)
#endif
    for(int j = 0; j < ht; j++)
      for(int i = 0; i < wd; i++)
        for(int k = 0; k < ch; k++)
          out[4 * ((size_t)j * wd + i) + k] = (in[(size_t)j * stride + (size_t)ch * i + k] - black) * scale;
    return;
  }
  int ii = 0, jj = 0;
  int si = 4, sj = wd * 4;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    sj = 4;
    si = ht * 4;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(wd, ch, scale, black, stride, ht) \
  shared(in, out, jj, ii, sj, si) \
  schedule(static)
#endif
  for(int j = 0; j < ht; j++)
  {
    float *out2 = out + (size_t)labs(sj) * jj + (size_t)labs(si) * ii + sj * j;
    const uint8_t *in2 = in + (size_t)stride * j;
    for(int i = 0; i < wd; i++)
    {
      for(int k = 0; k < ch; k++) out2[k] = (in2[k] - black) * scale;
      in2 += ch;
      out2 += si;
    }
  }
}

dt_imageio_retval_t dt_imageio_open_hdr(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(!buf)
    return DT_IMAGEIO_OK;

  dt_imageio_retval_t ret;

#ifdef HAVE_OPENEXR
  ret = dt_imageio_open_exr(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;
#endif

  ret = dt_imageio_open_rgbe(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

  ret = dt_imageio_open_pfm(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

#ifdef HAVE_LIBAVIF
  ret = dt_imageio_open_avif(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;
#endif

#ifdef HAVE_LIBHEIF
  ret = dt_imageio_open_heif(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;
#endif

  ret = dt_imageio_open_exotic(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

  return DT_IMAGEIO_FILE_CORRUPTED;
}

static const char *raster_formats[] = {
  ".jpg",  ".jpeg", ".png", ".tiff", ".tif", ".pgm", ".pbm", ".ppm",

#ifdef HAVE_OPENJPEG
  ".jp2",  ".j2k",
#endif

#ifdef HAVE_WEBP
  ".webp",
#endif

  NULL,
};

gboolean dt_imageio_is_raster(const char *filename)
{
  const char *c = filename + strlen(filename);
  while(c > filename && *c != '.') c--;
  if(*c != '.') return FALSE;

  int i = 0;
  while(raster_formats[i])
  {
    if(!strcasecmp(c, raster_formats[i])) return TRUE;
    i++;
  }

  return FALSE;
}

// We include DNG here since it's handled by raw libs
static const char *raw_formats[] = {
  ".3fr", ".ari", ".arw", ".bay", ".bmq", ".cap", ".cine", ".cr2", ".crw", ".cs1", ".dc2",
  ".dcr", ".dng", ".gpr", ".erf", ".fff", ".ia",  ".iiq",  ".k25", ".kc2", ".kdc", ".mdc",
  ".mef", ".mos", ".mrw", ".nef", ".nrw", ".orf", ".ori",  ".pef", ".pxn", ".qtk", ".raf",
  ".raw", ".rdc", ".rw2", ".rwl", ".sr2", ".srf", ".srw",  ".x3f",

#ifdef HAVE_LIBRAW
  ".cr3",
#endif

  NULL
};


gboolean dt_imageio_is_raw(const char *filename)
{
  const char *c = filename + strlen(filename);
  while(c > filename && *c != '.') c--;
  if(*c != '.') return FALSE;

  int i = 0;
  while(raw_formats[i])
  {
    if(!strcasecmp(c, raw_formats[i])) return TRUE;
    i++;
  }

  return FALSE;
}

static const char *hdr_formats[] = {
  ".pfm",  ".hdr",

#ifdef HAVE_OPENEXR
  ".exr",
#endif

#ifdef HAVE_LIBAVIF
  ".avif",
#endif

#ifdef HAVE_LIBHEIF
  ".heif", ".heic", ".hif",
#endif

  NULL
};


int dt_imageio_is_hdr(const char *filename)
{
  const char *c = filename + strlen(filename);
  while(c > filename && *c != '.') c--;
  if(*c != '.') return FALSE;

  int i = 0;
  while(hdr_formats[i])
  {
    if(!strcasecmp(c, hdr_formats[i])) return TRUE;
    i++;
  }

  return FALSE;
}

static gboolean _is_in_list(char *elem, char *list)
{
  // Search if elem is contained in the coma-separated list string
  gboolean success = FALSE;
  if(elem && list)
  {
    while(list != NULL && !success)
    {
      success = !g_ascii_strncasecmp(list, elem, strlen(elem));
      list = strtok(NULL, ",");
    }
  }

  return success;
}

gboolean dt_imageio_is_handled_by_libraw(dt_image_t *img, const char *filename)
{
  // Allow users to define some extensions, makers and models that should be handled by Libraw
  gboolean is_handled = FALSE;

  char *ext = g_strrstr(filename, ".") + 1; // move the pointer after the extension dot
  is_handled |= _is_in_list(ext,             strtok(dt_conf_get_string("libraw/extensions"), ","));
  is_handled |= _is_in_list(img->exif_maker, strtok(dt_conf_get_string("libraw/makers"), ","));
  is_handled |= _is_in_list(img->exif_model, strtok(dt_conf_get_string("libraw/models"), ","));

  const char *iolib = (is_handled) ? "Libraw" : "Rawspeed";
  dt_print(DT_DEBUG_IMAGEIO, "[image I/O] image `%s` from camera `%s` of maker `%s` loaded with %s\n", filename,
           img->exif_model, img->exif_maker, iolib);

  return is_handled;
}

// transparent read method to load ldr image to dt_raw_image_t with exif and so on.
dt_imageio_retval_t dt_imageio_open_raster(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(!buf)
    return DT_IMAGEIO_OK;

  dt_imageio_retval_t ret;

  ret = dt_imageio_open_jpeg(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

  ret = dt_imageio_open_tiff(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

#ifdef HAVE_WEBP
  ret = dt_imageio_open_webp(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;
#endif

  ret = dt_imageio_open_png(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

#ifdef HAVE_OPENJPEG
  ret = dt_imageio_open_j2k(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;
#endif

  ret = dt_imageio_open_pnm(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

  ret = dt_imageio_open_exotic(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

  return DT_IMAGEIO_FILE_CORRUPTED;
}

dt_imageio_retval_t dt_imageio_open_raw(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(!buf)
    return DT_IMAGEIO_OK;

  dt_imageio_retval_t ret;

  /* check if user wants to force processing through Libraw */
  const gboolean force_libraw = dt_imageio_is_handled_by_libraw(img, filename);

  /* use rawspeed to load the raw */
  if(!force_libraw)
  {
    ret = dt_imageio_open_rawspeed(img, filename, buf);
    if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
      return ret;
  }

#ifdef HAVE_LIBRAW
  /* fallback that tries to open file via LibRAW to support Canon CR3 */
  ret = dt_imageio_open_libraw(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;
#endif

  /* try Rawspeed again in case Libraw was forced but failed */
  if(force_libraw)
  {
    ret = dt_imageio_open_rawspeed(img, filename, buf);
    if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
      return ret;
  }

  /* fallback that tries to open file via GraphicsMagick */
  ret = dt_imageio_open_exotic(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
    return ret;

  return DT_IMAGEIO_FILE_CORRUPTED;
}

void dt_imageio_to_fractional(float in, uint32_t *num, uint32_t *den)
{
  if(!(in >= 0))
  {
    *num = *den = 0;
    return;
  }
  *den = 1;
  *num = (int)(in * *den + .5f);
  while(fabsf(*num / (float)*den - in) > 0.001f)
  {
    *den *= 10;
    *num = (int)(in * *den + .5f);
  }
}

int dt_imageio_export(const int32_t imgid, const char *filename, dt_imageio_module_format_t *format,
                      dt_imageio_module_data_t *format_params, const gboolean high_quality,
                      const gboolean copy_metadata, const gboolean export_masks,
                      dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
                      dt_iop_color_intent_t icc_intent, dt_imageio_module_storage_t *storage,
                      dt_imageio_module_data_t *storage_params, int num, int total, dt_export_metadata_t *metadata)
{
  if(strcmp(format->mime(format_params), "x-copy") == 0)
    /* This is a just a copy, skip process and just export */
    return format->write_image(format_params, filename, NULL, icc_type, icc_filename, NULL, 0, imgid, num, total, NULL,
                               export_masks);
  else
  {
    const gboolean is_scaling =
      dt_conf_is_equal("plugins/lighttable/export/resizing", "scaling");

    return dt_imageio_export_with_flags(imgid, filename, format, format_params, FALSE, FALSE, TRUE, is_scaling,
                                        FALSE, NULL, copy_metadata, export_masks, icc_type, icc_filename, icc_intent,
                                        storage, storage_params, num, total, metadata);
  }
}

gboolean _apply_style_before_export(dt_develop_t *dev, dt_imageio_module_data_t *format_params, const int32_t imgid)
{
  GList *style_items = dt_styles_get_item_list(format_params->style, TRUE, -1);
  if(!style_items)
  {
    dt_control_log(_("cannot find the style '%s' to apply during export."), format_params->style);
    return TRUE;
  }

  GList *modules_used = NULL;

  dt_ioppr_check_iop_order(dev, imgid, "dt_imageio_export_with_flags");
  dt_dev_pop_history_items_ext(dev);
  dt_ioppr_update_for_style_items(dev, style_items, TRUE);

  for(GList *st_items = style_items; st_items; st_items = g_list_next(st_items))
  {
    dt_style_item_t *st_item = (dt_style_item_t *)st_items->data;
    dt_styles_apply_style_item(dev, st_item, &modules_used);
  }

  g_list_free(modules_used);
  g_list_free_full(style_items, dt_style_item_free);

  return FALSE;
}

void _print_export_debug(dt_dev_pixelpipe_t *pipe, dt_imageio_module_data_t *format_params, const gboolean use_style)
{
  if(darktable.unmuted & DT_DEBUG_IMAGEIO)
  {
    fprintf(stderr,"[dt_imageio_export_with_flags] ");
    if(use_style)
    {
      fprintf(stderr,"appending style `%s'\n", format_params->style);
    }
    else fprintf(stderr,"\n");
    int cnt = 0;
    for(GList *nodes = pipe->nodes; nodes; nodes = g_list_next(nodes))
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
      if(piece->enabled)
      {
        cnt++;
        fprintf(stderr," %s", piece->module->op);
      }
    }
    fprintf(stderr," (%i)\n", cnt);
  }
}


void _filter_pipeline(const char *filter, dt_dev_pixelpipe_t *pipe)
{
  // Warning: can only filter prior to or past to a certain module, but not both !!!
  if(filter)
  {
    if(!strncmp(filter, "pre:", 4)) dt_dev_pixelpipe_disable_after(pipe, filter + 4);
    if(!strncmp(filter, "post:", 5)) dt_dev_pixelpipe_disable_before(pipe, filter + 5);
  }
}


gboolean _get_export_size(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe,
                          const dt_imageio_module_data_t *format_params, const gboolean is_scaling, double *scale,
                          int width, int height, int *processed_width, int *processed_height)
{
  const double image_ratio = (double)pipe->processed_width / (double)pipe->processed_height;

  if(is_scaling)
  {
    double _num, _denum;
    dt_imageio_resizing_factor_get_and_parsing(&_num, &_denum);
    const double scale_factor = _num / _denum;
    *scale = fmin(scale_factor, 1.);
    *processed_height = (int)roundf(pipe->processed_height * (*scale));
    *processed_width = (int)roundf(pipe->processed_width * (*scale));
    return 0;
  }

  // if width and height are both 0, we use the full resolution of the image
  if(width == 0 && height == 0)
  {
    // original resolution
    *processed_width = pipe->processed_width;
    *processed_height = pipe->processed_height;
    *scale = 1.;
    return 0;
  }

  if(width > 0 && height > 0)
  {
    // fixed width and height : fit within a bounding box
    *processed_width = MIN(pipe->processed_width, width);
    *processed_height = MIN(pipe->processed_height, height);
    double scale_x = (double)*processed_width / (double)pipe->processed_width;
    double scale_y = (double)*processed_height / (double)pipe->processed_height;
    *scale = fmin(scale_x, scale_y);

    // Note : we handle each case separately to avoid rounding errors
    if(image_ratio > 1.0)
    {
      // landscape image, width is the limiting factor
      *processed_width = MIN((int)roundf(pipe->processed_width * (*scale)), pipe->processed_width);
      *processed_height = MIN((int)roundf(*processed_width / image_ratio), pipe->processed_height);
    }
    else if(image_ratio < 1.0)
    {
      // portrait image, height is the limiting factor
      *processed_height = MIN((int)roundf(pipe->processed_height * (*scale)), pipe->processed_height);
      *processed_width = MIN((int)roundf(*processed_height * image_ratio), pipe->processed_width);
    }
    else
    {
      // square image, both width and height are the limiting factors
      *processed_width = MIN((int)roundf(pipe->processed_width * (*scale)), pipe->processed_width);
      *processed_height = MIN((int)roundf(pipe->processed_height * (*scale)), pipe->processed_height);
    }
    return 0;
  }

  if(width > 0)
  {
    // fluid height, fixed width
    *processed_width = MIN(pipe->processed_width, width);
    *processed_height = (int)roundf(*processed_width / image_ratio);
  }
  else if(height > 0)
  {
    // fluid width, fixed height
    *processed_height = MIN(pipe->processed_height, height);
    *processed_width = (int)roundf(*processed_height * image_ratio);
  }

  double scale_x = (double)*processed_width / (double)pipe->processed_width;
  double scale_y = (double)*processed_height / (double)pipe->processed_height;
  *scale = fmin(scale_x, scale_y);

  return 0;
}


void _clamp_float_to_uint8(const float *const inbuf, uint8_t *const outbuf, const size_t processed_width,
                           const size_t processed_height)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(processed_width, processed_height, inbuf, outbuf) \
  schedule(static)
#endif
  for(size_t k = 0; k < processed_width * processed_height; k++)
    for_four_channels(c)
      outbuf[4 * k + c] = (uint8_t)CLAMPF(roundf(inbuf[4 * k + c] * 255.f), 0.f, 255.f);
}


void _swap_byteorder_float_to_uint8(const float *const restrict inbuf, uint8_t *const outbuf,
                                    const size_t processed_width, const size_t processed_height)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(processed_width, processed_height, inbuf, outbuf) \
  schedule(static)
#endif
  for(size_t k = 0; k < processed_width * processed_height; k++)
  {
    outbuf[4 * k + 0] = (uint8_t)CLAMPF(roundf(inbuf[4 * k + 2] * 255.f), 0.f, 255.f);
    outbuf[4 * k + 1] = (uint8_t)CLAMPF(roundf(inbuf[4 * k + 1] * 255.f), 0.f, 255.f);
    outbuf[4 * k + 2] = (uint8_t)CLAMPF(roundf(inbuf[4 * k + 0] * 255.f), 0.f, 255.f);
    outbuf[4 * k + 3] = (uint8_t)CLAMPF(roundf(inbuf[4 * k + 3] * 255.f), 0.f, 255.f);
  }
}


void _export_final_buffer_to_uint16(const float *const restrict inbuf, uint16_t *const outbuf,
                                    const size_t processed_width, const size_t processed_height)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(processed_width, processed_height, inbuf, outbuf) \
  schedule(static)
#endif
  for(size_t k = 0; k < processed_width * processed_height; k++)
    for_four_channels(c)
      outbuf[4 * k + c] = (uint16_t)CLAMP(roundf(inbuf[4 * k + c] * 65535.f), 0.f, 65535.f);
}

void _export_apply_lua_actions(const int32_t imgid, const char *filename, dt_imageio_module_format_t *format,
                               dt_imageio_module_data_t *format_params, dt_imageio_module_storage_t *storage,
                               dt_imageio_module_data_t *storage_params)
{
#ifdef USE_LUA
    //Synchronous calling of lua intermediate-export-image events
    dt_lua_lock();

    lua_State *L = darktable.lua_state.state;

    luaA_push(L, dt_lua_image_t, &imgid);

    lua_pushstring(L, filename);

    luaA_push_type(L, format->parameter_lua_type, format_params);

    if(storage)
      luaA_push_type(L, storage->parameter_lua_type, storage_params);
    else
      lua_pushnil(L);

    dt_lua_event_trigger(L, "intermediate-export-image", 4);

    dt_lua_unlock();
#endif
}


// internal function: to avoid exif blob reading + 8-bit byteorder flag + high-quality override
int dt_imageio_export_with_flags(const int32_t imgid, const char *filename,
                                 dt_imageio_module_format_t *format, dt_imageio_module_data_t *format_params,
                                 const gboolean ignore_exif, const gboolean display_byteorder,
                                 const gboolean high_quality, gboolean is_scaling, const gboolean thumbnail_export,
                                 const char *filter, const gboolean copy_metadata, const gboolean export_masks,
                                 dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
                                 dt_iop_color_intent_t icc_intent, dt_imageio_module_storage_t *storage,
                                 dt_imageio_module_data_t *storage_params, int num, int total,
                                 dt_export_metadata_t *metadata)
{
  dt_times_t start;
  dt_get_times(&start);

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_t *cache = darktable.mipmap_cache;
  void *outbuf = NULL;

  // Get the history, aka sequence of editing changes
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, imgid);
  dt_ioppr_resync_modules_order(&dev);

  // Apply styles on top of current history
  const gboolean use_style = !thumbnail_export && format_params->style[0] != '\0';
  //  If a style is to be applied during export, add the iop params into the history
  if(use_style && _apply_style_before_export(&dev, format_params, imgid))
    goto error;

  int width = MAX(format_params->max_width, 0);
  int height = MAX(format_params->max_height, 0);
  double scale = 1.;

  // Get a pipeline, aka sequence of nodes
  int res = 0;
  dt_dev_pixelpipe_t pipe;
  if(thumbnail_export)
    res = dt_dev_pixelpipe_init_thumbnail(&pipe);
  else
    res = dt_dev_pixelpipe_init_export(&pipe, format->levels(format_params), export_masks);

  if(!res) goto error;

  dt_dev_pixelpipe_create_nodes(&pipe, &dev);

  // Sync history with pipeline nodes
  // Update the ICC type if DT_COLORSPACE_NONE is passed
  dt_colorspaces_get_output_profile(imgid, &icc_type, icc_filename);
  dt_dev_pixelpipe_set_icc(&pipe, icc_type, icc_filename, icc_intent);
  
  // Find out what input size we want
  dt_mipmap_size_t size = DT_MIPMAP_FULL;
  if(thumbnail_export)
  {
    // Init size with full-resolution raw
    dt_dev_pixelpipe_set_input(&pipe, &dev, imgid, width, height, size);
    dt_dev_pixelpipe_synch_all(&pipe, &dev);

    // Test if using the half-sized raw as input would still give us enough pixels to cover the desired image surface
    int out_width, out_height;
    dt_dev_pixelpipe_get_roi_out(&pipe, &dev, cache->max_width[DT_MIPMAP_F], cache->max_height[DT_MIPMAP_F], &out_width, &out_height);

    // Only one dimension needs to be at least as large as the requested surface
    if(out_width <= width || out_height <= height)
      size = DT_MIPMAP_F;
  }

  // Take a local copy of the input buffer so we can release the mipmap cache lock immediately
  dt_mipmap_cache_get(cache, &buf, imgid, size, DT_MIPMAP_BLOCKING, 'r');

  if(!buf.buf || buf.width == 0 || buf.height == 0)
  {
    dt_mipmap_cache_release(cache, &buf);
    goto error;
  }

  const size_t buf_width = buf.width;
  const size_t buf_height = buf.height;
  dt_mipmap_cache_release(cache, &buf);

  // Update size with actual input and resync nodes
  dt_dev_pixelpipe_set_input(&pipe, &dev, imgid, buf_width, buf_height, size);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);

  // Write debug info to stdout
  _print_export_debug(&pipe, format_params, use_style);

  // Remove modules past or prior a certain one.
  // Useful for partial exports, for technical purposes (HDR merge)
  _filter_pipeline(filter, &pipe);

  // Get theoritical final size of image, taking distortions and croppings AND borders into account,
  // considering full-size original input. Meaning we can enlarge or reduce the original image,
  // even taking full-res input.
  // Needs to be done after optional filtering, in case we filter out distortion modules
  dt_dev_pixelpipe_get_roi_out(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width,
                               &pipe.processed_height);

  dt_show_times(&start, "[export] creating pixelpipe");

  // Compute the actual final sizes that fit within the bounding box width*height
  // while preserving original image ratio
  int processed_width = 0;
  int processed_height = 0;
  _get_export_size(&dev, &pipe, format_params, is_scaling, &scale, width, height,
                   &processed_width, &processed_height);

  dt_print(DT_DEBUG_IMAGEIO,
           "[dt_imageio_export] (direct) image input %ix%i, turned to output %ix%i, will be exported to fit %ix%i "
           "--> final size is %ix%i\n",
           pipe.iwidth, pipe.iheight, pipe.processed_width, pipe.processed_height, width, height, processed_width,
           processed_height);


  const int bpp = format->bpp(format_params);

  dt_iop_roi_t roi = (dt_iop_roi_t){ 0, 0, processed_width, processed_height, scale };

  dt_get_times(&start);
  int err = dt_dev_pixelpipe_process(&pipe, &dev, roi);
  dt_show_times(&start, thumbnail_export ? "[dev_process_thumbnail] pixel pipeline processing thread"
                                         : "[dev_process_export] pixel pipeline processing thread");

  if(pipe.backbuf.hash == -1 || err)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dt_imageio_export_with_flags] no valid output buffer\n");
    goto error;
  }

  struct dt_pixel_cache_entry_t *cache_entry;
  void *data = NULL;
  if(!dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, pipe.backbuf.hash, &data, NULL, &cache_entry))
    goto error;
  
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, TRUE, cache_entry);

  // Down-conversion to low-precision formats:
  const size_t pixels = pipe.backbuf.width * pipe.backbuf.height * 4;
  if(bpp == 8)
  {
    outbuf = dt_pixelpipe_cache_alloc_align_cache(
        sizeof(uint8_t) * pixels,
        0);
    if(outbuf && display_byteorder)
      _swap_byteorder_float_to_uint8(data, outbuf, pipe.backbuf.width, pipe.backbuf.height);
    else if(outbuf)
      _clamp_float_to_uint8(data, outbuf, pipe.backbuf.width, pipe.backbuf.height);
  }
  else if(bpp == 16)
  {
    outbuf = dt_pixelpipe_cache_alloc_align_cache(
        sizeof(uint16_t) * pixels,
        0);
    if(outbuf)
      _export_final_buffer_to_uint16(data, outbuf, pipe.backbuf.width, pipe.backbuf.height);
  }
  else // output float, no further harm done to the pixels :)
  {
    outbuf = dt_pixelpipe_cache_alloc_align_cache(
        sizeof(float_t) * pixels,
        0);
    if(outbuf)
      memcpy(outbuf, data, sizeof(float_t) * pixels);
  }

  // Decrease ref count on the cache entry and release the read lock
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, 0, FALSE, cache_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, cache_entry);

  if(outbuf == NULL) goto error;

  format_params->width = pipe.backbuf.width;
  format_params->height = pipe.backbuf.height;

  // Exif data should be 65536 bytes max, but if original size is close to that,
  // adding new tags could make it go over that... so let it be and see what
  // happens when we write the image
  int length = 0;
  uint8_t *exif_profile = NULL;

  if(!ignore_exif)
  {
    gboolean from_cache = TRUE;
    char pathname[PATH_MAX] = { 0 };
    dt_image_full_path(imgid,  pathname,  sizeof(pathname),  &from_cache, __FUNCTION__);
    // find output color profile for this image:
    int sRGB = (icc_type == DT_COLORSPACE_SRGB);
    // last param is dng mode, it's false here
    length = dt_exif_read_blob(&exif_profile, pathname, imgid, sRGB, pipe.backbuf.width, pipe.backbuf.height, 0);
  }

  // Finally: write image buffer to target container
  res = format->write_image(format_params, filename, outbuf, icc_type, icc_filename, exif_profile, length, imgid,
                            num, total, &pipe, export_masks);

  if(exif_profile) free(exif_profile);
  if(res) goto error;

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);

  /* now write xmp into that container, if possible */
  if(copy_metadata && (format->flags(format_params) & FORMAT_FLAGS_SUPPORT_XMP))
    dt_exif_xmp_attach_export(imgid, filename, metadata);

  if(!thumbnail_export && strcmp(format->mime(format_params), "memory")
    && !(format->flags(format_params) & FORMAT_FLAGS_NO_TMPFILE))
  {
    _export_apply_lua_actions(imgid, filename, format, format_params, storage, storage_params);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_EXPORT_TMPFILE, imgid, filename, format,
                            format_params, storage, storage_params);
  }

  dt_pixelpipe_cache_free_align(outbuf);
  return 0; // success

error:
  dt_pixelpipe_cache_free_align(outbuf);
  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  return 1;
}


// fallback read method in case file could not be opened yet.
// use GraphicsMagick (if supported) to read exotic LDRs
dt_imageio_retval_t dt_imageio_open_exotic(dt_image_t *img, const char *filename,
                                           dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(!buf)
    return DT_IMAGEIO_OK;

  dt_imageio_retval_t ret;

#if defined(HAVE_GRAPHICSMAGICK)
  ret = dt_imageio_open_gm(img, filename, buf);
#elif defined(HAVE_IMAGEMAGICK)
  ret = dt_imageio_open_im(img, filename, buf);
#else
  ret = DT_IMAGEIO_FILE_CORRUPTED;
#endif

  return ret;
}

void dt_imageio_update_monochrome_workflow_tag(int32_t id, int mask)
{
  if(mask & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_PREVIEW | DT_IMAGE_MONOCHROME_BAYER))
  {
    guint tagid = 0;
    char tagname[64];
    snprintf(tagname, sizeof(tagname), "darktable|mode|monochrome");
    dt_tag_new(tagname, &tagid);
    dt_tag_attach(tagid, id, FALSE, FALSE);
  }
  else
    dt_tag_detach_by_string("darktable|mode|monochrome", id, FALSE, FALSE);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

void dt_imageio_set_hdr_tag(dt_image_t *img)
{
  guint tagid = 0;
  char tagname[64];
  snprintf(tagname, sizeof(tagname), "darktable|mode|hdr");
  dt_tag_new(tagname, &tagid);
  dt_tag_attach(tagid, img->id, FALSE, FALSE);
  img->flags |= DT_IMAGE_HDR;
  img->flags &= ~DT_IMAGE_LDR;
}

// =================================================
//   combined reading
// =================================================

dt_imageio_retval_t dt_imageio_open(dt_image_t *img,               // non-const * means you hold a write lock!
                                    const char *filename,          // full path
                                    dt_mipmap_buffer_t *buf)
{
  /* first of all, check if file exists, don't bother to test loading if not exists */
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
    return !DT_IMAGEIO_OK;

  const int32_t was_hdr = (img->flags & DT_IMAGE_HDR);
  const int32_t was_bw = dt_image_monochrome_flags(img);

  dt_imageio_retval_t ret = DT_IMAGEIO_FILE_CORRUPTED;
  img->loader = LOADER_UNKNOWN;

  // Start with extensions that are supposed to work.
  // If they don't, they are corrupted.

  if(dt_imageio_is_raster(filename))
  {
    ret = dt_imageio_open_raster(img, filename, buf);
    if(ret != DT_IMAGEIO_OK)
    {
      fprintf(stderr, "[imageio] The file %s is corrupted. Abort.\n", filename);
      dt_control_log(_("The file `%s` is corrupted."), filename);
      return ret;
    }
  }

  if(dt_imageio_is_raw(filename))
  {
    ret = dt_imageio_open_raw(img, filename, buf);
    if(ret != DT_IMAGEIO_OK)
    {
      fprintf(stderr, "[imageio] The file %s is corrupted. Abort.\n", filename);
      dt_control_log(_("The file `%s` is corrupted."), filename);
      return ret;
    }
  }

  if(dt_imageio_is_hdr(filename))
  {
    ret = dt_imageio_open_hdr(img, filename, buf);
    if(ret != DT_IMAGEIO_OK)
    {
      fprintf(stderr, "[imageio] The file %s is corrupted. Abort.\n", filename);
      dt_control_log(_("The file `%s` is corrupted."), filename);
      return ret;
    }
  }

  // fallback: bruteforce everything hoping for a miracle
  // Most likely, it's a format we never heard of.
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_raster(img, filename, buf);

  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_raw(img, filename, buf);

  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_hdr(img, filename, buf);

  // Final check and abort
  if(ret != DT_IMAGEIO_OK)
  {
    fprintf(stderr, "[imageio] The file %s is supported by none of our decoders.\n", filename);
    dt_control_log(_("The file `%s` is supported by none of our decoders."), filename);
    return ret;
  }

  if((ret == DT_IMAGEIO_OK) && !was_hdr && (img->flags & DT_IMAGE_HDR))
    dt_imageio_set_hdr_tag(img);

  if((ret == DT_IMAGEIO_OK) && (was_bw != dt_image_monochrome_flags(img)))
    dt_imageio_update_monochrome_workflow_tag(img->id, dt_image_monochrome_flags(img));

  img->p_width = img->width - img->crop_x - img->crop_width;
  img->p_height = img->height - img->crop_y - img->crop_height;

  return ret;
}

gboolean dt_imageio_lookup_makermodel(const char *maker, const char *model,
                                      char *mk, int mk_len, char *md, int md_len,
                                      char *al, int al_len)
{
  // At this stage, we can't tell which loader is used to open the image.
  gboolean found = dt_rawspeed_lookup_makermodel(maker, model,
                                                 mk, mk_len,
                                                 md, md_len,
                                                 al, al_len);
  if(found == FALSE)
  {
    // Special handling for CR3 raw files via libraw
    found = dt_libraw_lookup_makermodel(maker, model,
                                        mk, mk_len,
                                        md, md_len,
                                        al, al_len);
  }
  return found;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
