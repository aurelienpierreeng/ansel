/*
    This file is part of darktable,
    Copyright (C) 2010-2011, 2014 Henrik Andersson.
    Copyright (C) 2010-2012, 2014 johannes hanika.
    Copyright (C) 2011 Jonathan A. Kollasch.
    Copyright (C) 2011-2012, 2014, 2016-2018 Tobias Ellinghaus.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2019 Ulrich Pegelow.
    Copyright (C) 2013-2014, 2016 Roman Lebedev.
    Copyright (C) 2014 Edouard Gomez.
    Copyright (C) 2014 Pascal de Bruijn.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2017 luzpaz.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2020 Aurélien PIERRE.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Miloš Komarčević.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2023 Alynx Zhou.
    
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
#include "colorprofiles/colorspaces.h"
#include "imageio_tiff.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "metadata/exif.h"
#include "develop/develop.h"

#include <inttypes.h>
#include <memory.h>
#include <stdio.h>
#include <strings.h>
#include <tiffio.h>

#define LAB_CONVERSION_PROFILE DT_COLORSPACE_LIN_REC2020

typedef struct tiff_t
{
  TIFF *tiff;
  uint32_t width;
  uint32_t height;
  uint16_t bpp;
  uint16_t spp;
  uint16_t sampleformat;
  uint32_t scanlinesize;
  dt_image_t *image;
  float *mipbuf;
  tdata_t buf;
} tiff_t;

typedef union fp32_t
{
  uint32_t u;
  float f;
} fp32_t;

static inline float _half_to_float(uint16_t h)
{
  /* see https://en.wikipedia.org/wiki/Half-precision_floating-point_format#Exponent_encoding
     and https://en.wikipedia.org/wiki/Single-precision_floating-point_format#Exponent_encoding */

  /* TODO: use intrinsics when possible */

  /* from https://gist.github.com/rygorous/2156668 */
  static const fp32_t magic = { 113 << 23 };
  static const uint32_t shifted_exp = 0x7c00 << 13; // exponent mask after shift
  fp32_t o;

  o.u = (h & 0x7fff) << 13;     // exponent/mantissa bits
  uint32_t exp = shifted_exp & o.u;   // just the exponent
  o.u += (127 - 15) << 23;        // exponent adjust

  // handle exponent special cases
  if (exp == shifted_exp) // Inf/NaN?
    o.u += (128 - 16) << 23;    // extra exp adjust
  else if (exp == 0) // Zero/Denormal?
  {
    o.u += 1 << 23;             // extra exp adjust
    o.f -= magic.f;             // renormalize
  }

  o.u |= (h & 0x8000) << 16;    // sign bit
  return o.f;
}

static inline int _read_chunky_8(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    uint8_t *in = ((uint8_t *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      /* set rgb to first sample from scanline */
      out[0] = ((float)in[0]) * (1.0f / 255.0f);

      if(t->spp == 1)
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = ((float)in[1]) * (1.0f / 255.0f);
        out[2] = ((float)in[2]) * (1.0f / 255.0f);
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_chunky_16(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    uint16_t *in = ((uint16_t *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = ((float)in[0]) * (1.0f / 65535.0f);

      if(t->spp == 1)
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = ((float)in[1]) * (1.0f / 65535.0f);
        out[2] = ((float)in[2]) * (1.0f / 65535.0f);
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_chunky_h(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    uint16_t *in = ((uint16_t *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = _half_to_float(in[0]);

      if(t->spp == 1)
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = _half_to_float(in[1]);
        out[2] = _half_to_float(in[2]);
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_chunky_f(tiff_t *t)
{
  for(uint32_t row = 0; row < t->height; row++)
  {
    float *in = ((float *)t->buf);
    float *out = ((float *)t->mipbuf) + (size_t)4 * row * t->width;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) return -1;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = in[0];

      if(t->spp == 1)
      {
        out[1] = out[2] = out[0];
      }
      else
      {
        out[1] = in[1];
        out[2] = in[2];
      }

      out[3] = 0;
    }
  }

  return 1;
}

static inline int _read_chunky_8_Lab(tiff_t *t, uint16_t photometric)
{
  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_ROLE_ANY)->profile;
  const cmsHPROFILE output_profile = dt_colorspaces_get_profile(LAB_CONVERSION_PROFILE, "", DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR)->profile;
  const cmsHTRANSFORM xform = cmsCreateTransform(Lab, TYPE_LabA_FLT, output_profile, TYPE_RGBA_FLT, INTENT_PERCEPTUAL, 0);

  for(uint32_t row = 0; row < t->height; row++)
  {
    uint8_t *in = ((uint8_t *)t->buf);
    float *output = ((float *)t->mipbuf) + (size_t)4 * row * t->width;
    float *out = output;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) goto failed;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = ((float)in[0]) * (100.0f/255.0f);

      if(t->spp == 1)
      {
        out[1] = out[2] = 0;
      }
      else
      {
        if(photometric == PHOTOMETRIC_CIELAB)
        {
          out[1] = ((float)((int8_t)in[1]));
          out[2] = ((float)((int8_t)in[2]));
        }
        else // photometric == PHOTOMETRIC_ICCLAB
        {
          out[1] = ((float)(in[1])) - 128.0f;
          out[2] = ((float)(in[2])) - 128.0f;
        }
      }

      out[3] = 0;
    }

    cmsDoTransform(xform, output, output, t->width);
  }

  cmsDeleteTransform(xform);

  return 1;

failed:
  cmsDeleteTransform(xform);
  return -1;
}


static inline int _read_chunky_16_Lab(tiff_t *t, uint16_t photometric)
{
  const cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_ROLE_ANY)->profile;
  const cmsHPROFILE output_profile = dt_colorspaces_get_profile(LAB_CONVERSION_PROFILE, "", DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR)->profile;
  const cmsHTRANSFORM xform = cmsCreateTransform(Lab, TYPE_LabA_FLT, output_profile, TYPE_RGBA_FLT, INTENT_PERCEPTUAL, 0);
  const float range = (photometric == PHOTOMETRIC_CIELAB) ? 65535.0f : 65280.0f;

  for(uint32_t row = 0; row < t->height; row++)
  {
    uint16_t *in = ((uint16_t *)t->buf);
    float *output = ((float *)t->mipbuf) + (size_t)4 * row * t->width;
    float *out = output;

    /* read scanline */
    if(TIFFReadScanline(t->tiff, in, row, 0) == -1) goto failed;

    for(uint32_t i = 0; i < t->width; i++, in += t->spp, out += 4)
    {
      out[0] = ((float)in[0]) * (100.0f/range);

      if(t->spp == 1)
      {
        out[1] = out[2] = 0;
      }
      else
      {
        if(photometric == PHOTOMETRIC_CIELAB)
        {
          out[1] = ((float)((int16_t)in[1])) / 256.0f;
          out[2] = ((float)((int16_t)in[2])) / 256.0f;
        }
        else // photometric == PHOTOMETRIC_ICCLAB
        {
          out[1] = (((float)(in[1])) - 32768.0f) / 256.0f;
          out[2] = (((float)(in[2])) - 32768.0f) / 256.0f;
        }
      }

      out[3] = 0;
    }

    cmsDoTransform(xform, output, output, t->width);
  }

  cmsDeleteTransform(xform);

  return 1;

failed:
  cmsDeleteTransform(xform);
  return -1;
}


static void _warning_error_handler(const char *type, const char* module, const char* fmt, va_list ap)
{
  fprintf(stderr, "[tiff_open] %s: %s: ", type, module);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

static void _warning_handler(const char* module, const char* fmt, va_list ap)
{
  if(dt_get_debug_flags() & DT_DEBUG_IMAGEIO)
  {
    _warning_error_handler("warning", module, fmt, ap);
  }
}

static void _error_handler(const char* module, const char* fmt, va_list ap)
{
  _warning_error_handler("error", module, fmt, ap);
}

dt_imageio_retval_t dt_imageio_open_tiff(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  // doing this once would be enough, but our imageio reading code is
  // compiled into dt's core and doesn't have an init routine.
  TIFFSetWarningHandler(_warning_handler);
  TIFFSetErrorHandler(_error_handler);

  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strncmp(ext, ".tif", 4) && strncmp(ext, ".TIF", 4) && strncmp(ext, ".tiff", 5)
     && strncmp(ext, ".TIFF", 5))
    return DT_IMAGEIO_FILE_CORRUPTED;
  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  tiff_t t;
  uint16_t config;
  uint16_t photometric;
  uint16_t inkset;

  t.image = img;

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  t.tiff = TIFFOpenW(wfilename, "rb");
  dt_free(wfilename);
#else
  t.tiff = TIFFOpen(filename, "rb");
#endif

  if(IS_NULL_PTR(t.tiff)) return DT_IMAGEIO_FILE_CORRUPTED;

  TIFFGetField(t.tiff, TIFFTAG_IMAGEWIDTH, &t.width);
  TIFFGetField(t.tiff, TIFFTAG_IMAGELENGTH, &t.height);
  TIFFGetField(t.tiff, TIFFTAG_BITSPERSAMPLE, &t.bpp);
  TIFFGetField(t.tiff, TIFFTAG_SAMPLESPERPIXEL, &t.spp);
  TIFFGetFieldDefaulted(t.tiff, TIFFTAG_SAMPLEFORMAT, &t.sampleformat);
  TIFFGetField(t.tiff, TIFFTAG_PLANARCONFIG, &config);
  TIFFGetField(t.tiff, TIFFTAG_PHOTOMETRIC, &photometric);
  TIFFGetField(t.tiff, TIFFTAG_INKSET, &inkset);

  if(inkset == INKSET_CMYK || inkset == INKSET_MULTIINK)
  {
    fprintf(stderr, "[tiff_open] error: CMYK (or multiink) TIFFs are not supported.\n");
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  if(TIFFRasterScanlineSize(t.tiff) != TIFFScanlineSize(t.tiff)) return DT_IMAGEIO_FILE_CORRUPTED;

  t.scanlinesize = TIFFScanlineSize(t.tiff);

  dt_print(DT_DEBUG_IMAGEIO, "[tiff_open] %dx%d %dbpp, %d samples per pixel.\n", t.width, t.height, t.bpp, t.spp);

  // we only support 8/16 and 32 bits per pixel formats.
  if(t.bpp != 8 && t.bpp != 16 && t.bpp != 32)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* we only support 1,3 or 4 samples per pixel */
  if(t.spp != 1 && t.spp != 3 && t.spp != 4)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* don't depend on planar config if spp == 1 */
  if(t.spp > 1 && config != PLANARCONFIG_CONTIG)
  {
    fprintf(stderr, "[tiff_open] error: PlanarConfiguration other than chunky is not supported.\n");
    TIFFClose(t.tiff);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* initialize cached image buffer */
  t.image->width = t.width;
  t.image->height = t.height;

  t.image->dsc.channels = 4;
  t.image->dsc.datatype = TYPE_FLOAT;
  t.image->dsc.bpp = 4 * sizeof(float);
  t.image->dsc.cst = IOP_CS_RGB;
  t.image->dsc.filters = 0u;

  // flag the image buffer properly depending on sample format
  if(t.sampleformat == SAMPLEFORMAT_IEEEFP)
  {
    // HDR TIFF
    t.image->flags &= ~DT_IMAGE_LDR;
    t.image->flags |= DT_IMAGE_HDR;
  }
  else
  {
    // LDR TIFF
    t.image->flags |= DT_IMAGE_LDR;
    t.image->flags &= ~DT_IMAGE_HDR;
  }

  if(photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB)
    t.image->dsc.cst = IOP_CS_LAB;

  t.image->flags &= ~DT_IMAGE_RAW;
  t.image->flags &= ~DT_IMAGE_S_RAW;
  t.image->loader = LOADER_TIFF;

  if(IS_NULL_PTR(mbuf))
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_OK;
  }

  t.mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, t.image);
  if(IS_NULL_PTR(t.mipbuf))
  {
    fprintf(stderr, "[tiff_open] error: could not alloc full buffer for image `%s'\n", t.image->filename);
    TIFFClose(t.tiff);
    return DT_IMAGEIO_CACHE_FULL;
  }

  if((t.buf = _TIFFmalloc(t.scanlinesize)) == NULL)
  {
    TIFFClose(t.tiff);
    return DT_IMAGEIO_CACHE_FULL;
  }

  int ok = 1;

  if((photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB) && t.bpp == 8 && t.sampleformat == SAMPLEFORMAT_UINT)
  {
    ok = _read_chunky_8_Lab(&t, photometric);
    t.image->dsc.cst = IOP_CS_LAB;
  }
  else if((photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB) && t.bpp == 16 && t.sampleformat == SAMPLEFORMAT_UINT)
  {
    ok = _read_chunky_16_Lab(&t, photometric);
    t.image->dsc.cst = IOP_CS_LAB;
  }
  else if(t.bpp == 8 && t.sampleformat == SAMPLEFORMAT_UINT)
    ok = _read_chunky_8(&t);
  else if(t.bpp == 16 && t.sampleformat == SAMPLEFORMAT_UINT)
    ok = _read_chunky_16(&t);
  else if(t.bpp == 16 && t.sampleformat == SAMPLEFORMAT_IEEEFP)
    ok = _read_chunky_h(&t);
  else if(t.bpp == 32 && t.sampleformat == SAMPLEFORMAT_IEEEFP)
    ok = _read_chunky_f(&t);
  else
  {
    fprintf(stderr, "[tiff_open] error: not a supported tiff image format.\n");
    ok = 0;
  }

  _TIFFfree(t.buf);
  TIFFClose(t.tiff);

  if(ok == 1)
  {
    return DT_IMAGEIO_OK;
  }
  else
    return DT_IMAGEIO_FILE_CORRUPTED;
}

int dt_imageio_tiff_read_profile(const char *filename, uint8_t **out)
{
  TIFF *tiff = NULL;
  uint32_t profile_len = 0;
  uint8_t *profile = NULL;
  uint16_t photometric;

  if(!(filename && *filename && out)) return 0;

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  tiff = TIFFOpenW(wfilename, "rb");
  dt_free(wfilename);
#else
  tiff = TIFFOpen(filename, "rb");
#endif

  if(IS_NULL_PTR(tiff)) return 0;

  TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric);

  if(photometric == PHOTOMETRIC_CIELAB || photometric == PHOTOMETRIC_ICCLAB)
  {
    profile = dt_colorspaces_get_profile(LAB_CONVERSION_PROFILE, "", DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR)->profile;

    cmsSaveProfileToMem(profile, 0, &profile_len);
    if(profile_len > 0)
    {
      *out = (uint8_t *)g_malloc(profile_len);
      cmsSaveProfileToMem(profile, *out, &profile_len);
    }
  }
  else if(TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &profile_len, &profile))
  {
    if(profile_len > 0)
    {
      *out = (uint8_t *)g_malloc(profile_len);
      memcpy(*out, profile, profile_len);
    }
  }
  else
    profile_len = 0;

  TIFFClose(tiff);

  return profile_len;
}

/* ---- Embedded-preview decoding, from a memory blob ------------------------
 *
 * A raw file that embeds a JPEG preview is handled by libjpeg in
 * dt_imageio_large_thumbnail(); one that embeds a TIFF preview lands here.
 * These previews are small, self-contained images, so the whole blob is
 * already in memory and libtiff reads it through TIFFClientOpen with the five
 * callbacks below -- no temporary file, and no second image library.
 *
 * Sample format and bit depth are libtiff's problem, not ours: the RGBA
 * interface converts whatever the preview holds into 8-bit RGBA, and the cases
 * it cannot convert are refused up front by TIFFRGBAImageOK(). See the depth
 * note in dt_imageio_tiff_decode_blob(). */

typedef struct _tiff_blob_t
{
  const uint8_t *data;
  tmsize_t size;
  // Held as toff_t, libtiff's file-offset type, rather than tmsize_t: a seek past the end is
  // legal (see _blob_seek) and must record the position asked for, which a corrupt offset in
  // the file's own tags can put far beyond the blob. _blob_read() is what bounds it.
  toff_t pos;
} _tiff_blob_t;

static tmsize_t _blob_read(thandle_t handle, void *buffer, tmsize_t size)
{
  _tiff_blob_t *blob = (_tiff_blob_t *)handle;
  // At or past the end reads as empty rather than as an error -- this is the single place the
  // extent of the data is enforced, so _blob_seek() does not have to refuse anything.
  if(size <= 0 || blob->pos >= (toff_t)blob->size) return 0;
  const tmsize_t available = (tmsize_t)((toff_t)blob->size - blob->pos);
  const tmsize_t n = (size < available) ? size : available;
  memcpy(buffer, blob->data + (size_t)blob->pos, (size_t)n);
  blob->pos += (toff_t)n;
  return n;
}

// Read-only: libtiff still requires a write callback, and refusing every write
// is what makes the handle read-only rather than silently corrupting anything.
static tmsize_t _blob_write(thandle_t handle, void *buffer, tmsize_t size)
{
  return 0;
}

static toff_t _blob_seek(thandle_t handle, toff_t offset, int whence)
{
  _tiff_blob_t *blob = (_tiff_blob_t *)handle;
  toff_t base = 0;
  switch(whence)
  {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = blob->pos; break;
    case SEEK_END: base = (toff_t)blob->size; break;
    default: return (toff_t)-1;
  }
  const toff_t target = base + offset;
  if(target < base) return (toff_t)-1; // wrapped: the caller asked for something absurd

  // Seeking beyond the end is legal and must succeed, exactly as lseek(2) does: libtiff probes
  // an offset before deciding whether to read it, and expects the new position back rather than
  // an error. Refusing here would turn a routine probe into a fatal read failure. The end of the
  // data is enforced by _blob_read(), which returns 0 bytes from any position at or past it.
  blob->pos = target;
  return target;
}

static int _blob_close(thandle_t handle)
{
  return 0;
}

static toff_t _blob_size(thandle_t handle)
{
  return (toff_t)((_tiff_blob_t *)handle)->size;
}

// No memory-mapped path: the blob is already a plain host buffer, and claiming
// otherwise would hand libtiff a mapping it would try to unmap.
static int _blob_map(thandle_t handle, void **base, toff_t *size)
{
  return 0;
}

static void _blob_unmap(thandle_t handle, void *base, toff_t size)
{
}

gboolean dt_imageio_tiff_decode_blob(const uint8_t *const blob, const size_t bufsize, uint8_t **out,
                                     int32_t *width, int32_t *height)
{
  if(IS_NULL_PTR(blob) || bufsize == 0 || IS_NULL_PTR(out) || IS_NULL_PTR(width) || IS_NULL_PTR(height))
    return FALSE;

  // Same handlers the file path installs, so a malformed preview is reported
  // through our log instead of libtiff's default stderr chatter.
  TIFFSetWarningHandler(_warning_handler);
  TIFFSetErrorHandler(_error_handler);

  _tiff_blob_t handle = { .data = blob, .size = (tmsize_t)bufsize, .pos = 0 };
  TIFF *tiff = TIFFClientOpen("embedded-preview", "rm", (thandle_t)&handle, _blob_read, _blob_write,
                              _blob_seek, _blob_close, _blob_size, _blob_map, _blob_unmap);
  if(IS_NULL_PTR(tiff)) return FALSE;

  gboolean ok = FALSE;
  uint32_t w = 0, h = 0;
  if(!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &w) || !TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &h)
     || w == 0 || h == 0)
    goto done;

  /* Bit depth is NOT assumed. TIFFReadRGBAImage() normalises 1-, 2-, 4-, 8- and 16-bit samples
   * down to 8 bits per channel itself, along with the photometric interpretation (palette,
   * greyscale, YCbCr, CMYK, ...) -- which is the whole reason for using the RGBA interface here
   * rather than reading strips by hand. What it does NOT handle is 32-bit integer or float
   * samples, and a few exotic compression/photometric combinations.
   *
   * TIFFRGBAImageOK() is libtiff's own predicate for exactly that question and fills in the
   * reason, so an unsupported preview is refused with a diagnosis instead of failing anonymously
   * inside the read below. Losing precision to 8 bits is correct here regardless: the caller's
   * contract is an 8-bit RGBx buffer for a thumbnail. */
  char why[1024] = { 0 };
  if(!TIFFRGBAImageOK(tiff, why))
  {
    dt_print(DT_DEBUG_IMAGEIO, "[tiff_decode_blob] embedded preview cannot be read as RGBA: %s\n", why);
    goto done;
  }

  uint16_t bps = 0;
  if(TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bps) && bps != 8)
    dt_print(DT_DEBUG_IMAGEIO, "[tiff_decode_blob] %u-bit embedded preview, converted to 8-bit\n",
             (unsigned)bps);

  // TIFFReadRGBAImage indexes its raster with a 32-bit pixel count, so refuse
  // anything that would overflow it rather than trusting the tags in a file we
  // did not write.
  if((uint64_t)w * (uint64_t)h > (uint64_t)0xFFFFFFFFu / 4u) goto done;

  const size_t npixels = (size_t)w * (size_t)h;
  uint8_t *pixels = (uint8_t *)dt_pixelpipe_cache_alloc_align_cache(sizeof(uint8_t) * 4 * npixels, 0);
  if(IS_NULL_PTR(pixels)) goto done;

  // ORIENTATION_TOPLEFT so the result is top-down like every other decoder here;
  // libtiff would otherwise hand back a bottom-up raster. stopOnError = 0: a
  // partially decodable preview is still worth showing.
  if(!TIFFReadRGBAImageOriented(tiff, w, h, (uint32_t *)pixels, ORIENTATION_TOPLEFT, 0))
  {
    dt_pixelpipe_cache_free_align(pixels);
    goto done;
  }

  // libtiff packs each pixel as one host-order uint32 (ABGR); unpack in place to the R, G, B,
  // unused byte layout the callers expect. Copied out through memcpy rather than read through a
  // uint32_t* view of a uint8_t buffer: the compiler turns it into the same single load, and it
  // keeps the loop from resting on an effective-type argument (the allocation has no declared
  // type, so libtiff's write is what makes it uint32) that a reader would have to reconstruct.
  for(size_t i = 0; i < npixels; i++)
  {
    uint8_t *const dest = pixels + 4 * i;
    uint32_t px;
    memcpy(&px, dest, sizeof(px));
    dest[0] = (uint8_t)TIFFGetR(px);
    dest[1] = (uint8_t)TIFFGetG(px);
    dest[2] = (uint8_t)TIFFGetB(px);
    dest[3] = 0;
  }

  *out = pixels;
  *width = (int32_t)w;
  *height = (int32_t)h;
  ok = TRUE;

done:
  TIFFClose(tiff);
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
