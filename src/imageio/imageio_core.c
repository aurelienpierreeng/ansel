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
#include "colorprofiles/colorspaces.h"
#include "common/image.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "system/macros.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "system/mem_alloc.h"
#include "system/simd.h"
#include "common/logging.h"
#include "common/times.h"
#include "common/paths.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "common/xmp_sidecar.h"
#include "metadata/exif.h"
#include "caches/image_cache.h"
#include "history/history.h"
#include "common/image_extensions.h"
#include "imageio/imageio_core.h"
#include "imageio/imageio_module.h"
#ifdef HAVE_OPENEXR
#include "imageio/imageio_exr.h"
#endif
#ifdef HAVE_OPENJPEG
#include "imageio/imageio_j2k.h"
#endif
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_pfm.h"
#include "imageio/imageio_png.h"
#include "imageio/imageio_pnm.h"
#include "imageio/imageio_rawspeed.h"
#include "imageio/imageio_libraw.h"
#include "imageio/imageio_rgbe.h"
#include "imageio/imageio_tiff.h"
#ifdef HAVE_LIBAVIF
#include "imageio/imageio_avif.h"
#endif
#ifdef HAVE_LIBHEIF
#include "imageio/imageio_heif.h"
#endif
#ifdef HAVE_WEBP
#include "imageio/imageio_webp.h"
#endif
#include "imageio/imageio_libraw.h"
#include "caches/mipmap_cache.h"
#include "common/styles.h"
#include "common/conf.h"
#include "control/user_message.h"
#include "develop/develop.h"
#include "develop/imageop.h"


#include <assert.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "imageio/imageio_profile.h"
#include "control/signal.h"


// Best-effort image-type hint from the file extension. Returns DT_IMAGE_RAW / DT_IMAGE_LDR /
// DT_IMAGE_HDR for the unambiguous extensions, or 0 ("unknown") for containers whose dynamic range
// only the decoder can settle (TIFF, AVIF, HEIF/HEIC, DNG -- see dt_image_ext_is_ambiguous()).
// Callers must treat 0 as "decode it"; dt_image_buffer_resolve_flags() sets the authoritative
// LDR/HDR/MOSAIC flags once decoded.
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
  else if(!strcmp(mime_type, "image/tiff") || !strcmp(mime_type, "image/x-tiff"))
  {
    // The only other preview format raw files actually use. See
    // dt_imageio_tiff_decode_blob() for why libtiff rather than a generic decoder.
    if(dt_imageio_tiff_decode_blob(buf, bufsize, buffer, th_width, th_height))
    {
      // TODO: check if the embedded thumbs have a color space set! currently we assume that it's always sRGB
      *color_space = DT_COLORSPACE_SRGB;
      res = 0;
    }
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
  dt_free(mime_type);
  dt_free(buf);
  return res;
}

gboolean dt_imageio_crop_thumbnail(const dt_boundingbox_t box, uint8_t *const buffer,
                                   int32_t *width, int32_t *height)
{
  if(IS_NULL_PTR(buffer) || IS_NULL_PTR(width) || IS_NULL_PTR(height)) return FALSE;

  const int32_t in_width = *width;
  const int32_t in_height = *height;
  if(in_width < 1 || in_height < 1) return FALSE;

  /* The box is normalized against the frame the camera renders its previews from, which is the
   * same DefaultCropOrigin/DefaultCropSize rectangle the tag itself references -- so it maps
   * onto the decoded preview directly, at whatever resolution that preview happens to be. */
  const int32_t left = CLAMP((int32_t)roundf(box[1] * in_width), 0, in_width - 1);
  const int32_t top = CLAMP((int32_t)roundf(box[0] * in_height), 0, in_height - 1);
  const int32_t right = CLAMP((int32_t)roundf(box[3] * in_width), left + 1, in_width);
  const int32_t bottom = CLAMP((int32_t)roundf(box[2] * in_height), top + 1, in_height);

  const int32_t out_width = right - left;
  const int32_t out_height = bottom - top;
  if(out_width == in_width && out_height == in_height) return FALSE;

  /* Compact in place, forward: destination row j starts at 4 * j * out_width, its source at
   * 4 * ((j + top) * in_width + left). Since out_width <= in_width and top, left >= 0, the
   * destination never runs ahead of the source, so no row is overwritten before it is read. */
  for(int32_t j = 0; j < out_height; j++)
  {
    const uint8_t *const source = buffer + 4 * ((size_t)(j + top) * in_width + left);
    uint8_t *const destination = buffer + 4 * (size_t)j * out_width;
    memmove(destination, source, 4 * (size_t)out_width);
  }

  *width = out_width;
  *height = out_height;
  return TRUE;
}

gboolean dt_imageio_has_mono_preview(const char *filename)
{
  dt_colorspaces_color_profile_type_t color_space;
  uint8_t *tmp = NULL;
  int32_t thumb_width = 0, thumb_height = 0;
  gboolean mono = FALSE;

  if(dt_imageio_large_thumbnail(filename, &tmp, &thumb_width, &thumb_height, &color_space, -1, -1))
    goto cleanup;
  if((thumb_width < 32) || (thumb_height < 32) || (IS_NULL_PTR(tmp)))
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

__DT_CLONE_TARGETS__
void dt_imageio_flip_buffers(char *out, const char *in, const size_t bpp, const int wd, const int ht,
                             const int fwd, const int fht, const int stride,
                             const dt_image_orientation_t orientation)
{
  if(!orientation)
  {
    __OMP_PARALLEL_FOR__()
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
  __OMP_PARALLEL_FOR__()
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
    __OMP_PARALLEL_FOR__()
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
  __OMP_PARALLEL_FOR__()
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
  if(IS_NULL_PTR(buf))
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

  return DT_IMAGEIO_FILE_CORRUPTED;
}

// Decoder routing: dt_image_ext_is_ldr() drives dt_imageio_open_raster(), the generic/
// exotic-capable chain -- kept distinct from is_hdr() (dt_imageio_open_hdr(), the dedicated
// OpenEXR/Radiance/PFM/AVIF/HEIF chain). See the comment on dt_image_ext_is_raw/_ldr/_hdr in
// common/image_extensions.h for why these must never be combined for routing purposes.
gboolean dt_imageio_is_raster(const char *filename)
{
  const char *c = filename + strlen(filename);
  while(c > filename && *c != '.') c--;
  if(*c != '.') return FALSE;
  return dt_image_ext_is_ldr(c + 1);
}

gboolean dt_imageio_is_raw(const char *filename)
{
  const char *c = filename + strlen(filename);
  while(c > filename && *c != '.') c--;
  if(*c != '.') return FALSE;
  return dt_image_ext_is_raw(c + 1);
}

int dt_imageio_is_hdr(const char *filename)
{
  const char *c = filename + strlen(filename);
  while(c > filename && *c != '.') c--;
  if(*c != '.') return FALSE;
  return dt_image_ext_is_hdr(c + 1);
}

static gboolean _is_in_list(char *elem, char *list)
{
  // Search if elem is contained in the coma-separated list string
  gboolean success = FALSE;
  if(elem && list)
  {
    while(!IS_NULL_PTR(list) && !success)
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
  char *extensions = dt_conf_get_string("libraw/extensions");
  char *makers = dt_conf_get_string("libraw/makers");
  char *models = dt_conf_get_string("libraw/models");

  is_handled |= _is_in_list(ext, strtok(extensions, ","));
  is_handled |= _is_in_list(img->exif_maker, strtok(makers, ","));
  is_handled |= _is_in_list(img->exif_model, strtok(models, ","));

  dt_free(extensions);
  dt_free(makers);
  dt_free(models);

  const char *iolib = (is_handled) ? "Libraw" : "Rawspeed";
  dt_print(DT_DEBUG_IMAGEIO, "[image I/O] image `%s` from camera `%s` of maker `%s` loaded with %s\n", filename,
           img->exif_model, img->exif_maker, iolib);

  return is_handled;
}

// transparent read method to load ldr image to dt_raw_image_t with exif and so on.
dt_imageio_retval_t dt_imageio_open_raster(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(IS_NULL_PTR(buf))
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

  return DT_IMAGEIO_FILE_CORRUPTED;
}

dt_imageio_retval_t dt_imageio_open_raw(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(IS_NULL_PTR(buf))
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
                                        storage, storage_params, num, total, metadata, NULL);
  }
}

gboolean _apply_style_before_export(dt_develop_t *dev, dt_imageio_module_data_t *format_params, const int32_t imgid)
{
  GList *style_items = dt_styles_get_item_list(format_params->style, TRUE, -1);
  if(IS_NULL_PTR(style_items))
  {
    dt_control_log(_("cannot find the style '%s' to apply during export."), format_params->style);
    return TRUE;
  }

  dt_ioppr_check_iop_order(dev, imgid, "dt_imageio_export_with_flags");
  dt_dev_pop_history_items(dev);
  dt_ioppr_update_for_style_items(dev, style_items, TRUE);

  for(GList *st_items = style_items; st_items; st_items = g_list_next(st_items))
  {
    dt_style_item_t *st_item = (dt_style_item_t *)st_items->data;
    dt_styles_apply_style_item(dev, st_item);
  }

  g_list_free_full(style_items, dt_style_item_free);
  style_items = NULL;

  return FALSE;
}

void _print_export_debug(dt_dev_pixelpipe_t *pipe, dt_imageio_module_data_t *format_params, const gboolean use_style)
{
  if(dt_get_debug_flags() & DT_DEBUG_IMAGEIO)
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
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < processed_width * processed_height; k++)
    for_four_channels(c)
      outbuf[4 * k + c] = (uint8_t)CLAMPF(roundf(inbuf[4 * k + c] * 255.f), 0.f, 255.f);
}


void _swap_byteorder_float_to_uint8(const float *const restrict inbuf, uint8_t *const outbuf,
                                    const size_t processed_width, const size_t processed_height)
{
  __OMP_PARALLEL_FOR__()
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
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < processed_width * processed_height; k++)
    for_four_channels(c)
      outbuf[4 * k + c] = (uint16_t)CLAMP(roundf(inbuf[4 * k + c] * 65535.f), 0.f, 65535.f);
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
                                 dt_export_metadata_t *metadata, dt_atomic_int *shutdown)
{
  dt_times_t start;
  dt_get_times(&start);

  dt_mipmap_buffer_t buf;
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
    res = dt_dev_pixelpipe_init_thumbnail(&pipe, &dev);
  else
    res = dt_dev_pixelpipe_init_export(&pipe, &dev, format->levels(format_params), export_masks);

  if(!res) goto error;

  pipe.shutdown_ext = shutdown;

  dt_dev_pixelpipe_create_nodes(&pipe);

  // Sync history with pipeline nodes
  // Update the ICC type if DT_COLORSPACE_NONE is passed
  dt_colorspaces_get_output_profile(imgid, &icc_type, icc_filename);
  dt_dev_pixelpipe_set_icc(&pipe, icc_type, icc_filename, icc_intent);
  
  // Find out what input size we want
  dt_mipmap_size_t size = DT_MIPMAP_FULL;

  // Take a local copy of the input buffer so we can release the mipmap cache lock immediately
  dt_mipmap_cache_get(&buf, imgid, size, DT_MIPMAP_BLOCKING, 'r');

  if(IS_NULL_PTR(buf.buf) || buf.width == 0 || buf.height == 0)
  {
    dt_mipmap_cache_release(&buf);
    goto error;
  }

  const size_t buf_width = buf.width;
  const size_t buf_height = buf.height;
  dt_mipmap_cache_release(&buf);

  // Update size with actual input and resync nodes
  dt_dev_pixelpipe_set_input(&pipe, imgid, buf_width, buf_height, buf.iscale, size);
  dt_dev_pixelpipe_synch_all(&pipe);

  // Write debug info to stdout
  _print_export_debug(&pipe, format_params, use_style);

  // Remove modules past or prior a certain one.
  // Useful for partial exports, for technical purposes (HDR merge)
  _filter_pipeline(filter, &pipe);

  // This export path drives the pipe directly instead of through dt_dev_pixelpipe_change(), so
  // establish the buffer-format contract (and disable incompatible nodes) explicitly, after the
  // optional filtering changed the node set and before ROI planning / global hashing.
  dt_dev_pixelpipe_propagate_formats(&pipe);

  // Get theoritical final size of image, taking distortions and croppings AND borders into account,
  // considering full-size original input. Meaning we can enlarge or reduce the original image,
  // even taking full-res input.
  // Needs to be done after optional filtering, in case we filter out distortion modules
  dt_dev_pixelpipe_get_roi_out(&pipe, pipe.iwidth, pipe.iheight, &pipe.processed_width,
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
  int err = dt_dev_pixelpipe_process(&pipe, roi);
  dt_show_times(&start, thumbnail_export ? "[dev_process_thumbnail] pixel pipeline processing thread"
                                         : "[dev_process_export] pixel pipeline processing thread");

  if(dt_dev_backbuf_get_hash(&pipe.backbuf) == -1 || err)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dt_imageio_export_with_flags] no valid output buffer\n");
    goto error;
  }

  struct dt_pixel_cache_entry_t *cache_entry;
  void *data = NULL;
  /* Atomically look up the final pipeline output and increment its refcount under the cache
   * mutex.  peek() + separate ref_count_entry() has a TOCTOU window: peek releases its
   * tryrdlock immediately and returns with no ownership, so a concurrent eviction thread
   * could see refcount==1 (backbuf keepalive only) and decrement it to 0 between peek()
   * returning and our ref_count_entry() call — leaving us with a dangling data pointer that
   * the OpenMP conversion threads then read → SIGSEGV.  ref_entry_by_hash() closes that
   * window by holding cache->lock across both the lookup and the increment. */
  if(!dt_dev_pixelpipe_cache_ref_entry_by_hash(dt_dev_backbuf_get_hash(&pipe.backbuf),
                                               &data, &cache_entry)
     || !data)
  {
    if(cache_entry)
      dt_dev_pixelpipe_cache_ref_count_entry(FALSE, cache_entry);
    goto error;
  }

  /* Hold a read lock for the duration of the conversion so no writer can replace the buffer
   * while the OpenMP threads are reading it. */
  dt_dev_pixelpipe_cache_rdlock_entry(TRUE, cache_entry);

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

    /* Thumbnail export stores the in-memory RGBA buffer straight into the mipmap cache.
     * The thumbnail pipeline does not maintain a meaningful alpha contract across all
     * modules, so random zero/garbage alpha values would make valid RGB thumbnails render
     * black in consumers that composite the mipmap buffer. Keep thumbnail alpha opaque at
     * the export boundary and leave RGB untouched. */
    if(outbuf && thumbnail_export)
    {
      uint8_t *thumbnail_buf = (uint8_t *)outbuf;
      __OMP_PARALLEL_FOR__()
      for(size_t k = 0; k < pixels / 4; k++) thumbnail_buf[4 * k + 3] = UINT8_MAX;
    }
  }
  else if(bpp == 16)
  {
    outbuf = dt_pixelpipe_cache_alloc_align_cache(
        sizeof(uint16_t) * pixels,
        0);
    if(outbuf)
      _export_final_buffer_to_uint16(data, outbuf, pipe.backbuf.width, pipe.backbuf.height);

    if(outbuf && thumbnail_export)
    {
      uint16_t *thumbnail_buf = (uint16_t *)outbuf;
      __OMP_PARALLEL_FOR__()
      for(size_t k = 0; k < pixels / 4; k++) thumbnail_buf[4 * k + 3] = UINT16_MAX;
    }
  }
  else // output float, no further harm done to the pixels :)
  {
    outbuf = dt_pixelpipe_cache_alloc_align_cache(
        sizeof(float_t) * pixels,
        0);
    if(outbuf)
      memcpy(outbuf, data, sizeof(float_t) * pixels);

    if(outbuf && thumbnail_export)
    {
      float *thumbnail_buf = (float *)outbuf;
      __OMP_PARALLEL_FOR__()
      for(size_t k = 0; k < pixels / 4; k++) thumbnail_buf[4 * k + 3] = 1.0f;
    }
  }

  // Decrease ref count on the cache entry and release the read lock
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, cache_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(FALSE, cache_entry);

  if(IS_NULL_PTR(outbuf)) goto error;

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

  dt_free(exif_profile);
  if(res) goto error;

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);

  /* now write xmp into that container, if possible */
  if(copy_metadata && (format->flags(format_params) & FORMAT_FLAGS_SUPPORT_XMP))
    dt_exif_xmp_attach_export(imgid, filename, metadata);

  if(!thumbnail_export && strcmp(format->mime(format_params), "memory")
    && !(format->flags(format_params) & FORMAT_FLAGS_NO_TMPFILE))
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_IMAGE_EXPORT_TMPFILE, imgid, filename, format,
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

  dt_imageio_retval_t ret = DT_IMAGEIO_FILE_CORRUPTED;
  img->loader = LOADER_UNKNOWN;

  // Start with the decoder(s) the extension routes to (fast path). Unlike before, a failure here
  // no longer aborts: an extension can route to more than one category (see
  // dt_image_ext_is_ambiguous() -- e.g. a linear DNG that a raw decoder can't demosaic, or a
  // renamed/mislabeled file), so let it cascade into the bruteforce section below instead of
  // reporting "corrupted" on a file a later attempt would have opened fine.
  if(dt_imageio_is_raster(filename))
    ret = dt_imageio_open_raster(img, filename, buf);

  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL && dt_imageio_is_raw(filename))
    ret = dt_imageio_open_raw(img, filename, buf);

  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL && dt_imageio_is_hdr(filename))
    ret = dt_imageio_open_hdr(img, filename, buf);

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

  img->p_width = img->width - img->crop_x - img->crop_width;
  img->p_height = img->height - img->crop_y - img->crop_height;

  // The codec has now populated img->dsc: finalize the buffer-derived type flags
  // (DT_IMAGE_MOSAIC + DT_IMAGE_BUFFER_RESOLVED). This is the single point where the
  // decoded descriptor is mapped to the persisted classification.
  dt_image_buffer_resolve_flags(img);

  return ret;
}

dt_imageio_retval_t dt_imageio_open_standalone(dt_image_t *img, const char *filename,
                                               dt_mipmap_buffer_t *buf)
{
  if(IS_NULL_PTR(buf)) return DT_IMAGEIO_LOAD_FAILED;

  memset(buf, 0, sizeof(*buf));
  buf->size = DT_MIPMAP_FULL;

  /* The decoder allocates into the entry it is handed. Owning one here is what lets callers
   * decode a file without knowing that a cache entry exists -- see the header. */
  buf->cache_entry = dt_cache_entry_new_detached();
  if(IS_NULL_PTR(buf->cache_entry)) return DT_IMAGEIO_CACHE_FULL;

  const dt_imageio_retval_t ret = dt_imageio_open(img, filename, buf);
  if(ret != DT_IMAGEIO_OK) dt_imageio_close_standalone(buf);
  return ret;
}

void dt_imageio_close_standalone(dt_mipmap_buffer_t *buf)
{
  if(IS_NULL_PTR(buf) || IS_NULL_PTR(buf->cache_entry)) return;
  dt_cache_entry_free_detached(buf->cache_entry);
  buf->cache_entry = NULL;
  buf->buf = NULL;
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
