/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

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

#include "canvas/canvas_export.h"

#include "canvas/canvas_format.h"
#include "canvas/canvas_paint.h"
#include "canvas/canvas_render.h"
#include "colorprofiles/colorspaces.h"
#include "common/pdf.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <cairo.h>
#include <glib/gstdio.h>
#include <jpeglib.h>
#include <lcms2.h>
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <tiffio.h>

/** More than this and the raster is a mistake, not a print. */
#define EXPORT_MAX_PIXELS (200u * 1024u * 1024u)
#define EXPORT_SURFACE_CACHE_BYTES (512u * 1024u * 1024u)

/* --- what a page is ------------------------------------------------------------------- */

/**
 * One sheet: the canvas rectangle it shows, and its size in points. With a bleed the sheet is
 * larger than the page on every side and the rectangle grows with it, so whatever the canvas
 * holds out there is drawn rather than a margin being left.
 */
typedef struct dt_canvas_export_page_t
{
  dt_canvas_rect_t area;
  double width_pt;
  double height_pt;
} dt_canvas_export_page_t;

dt_canvas_export_options_t dt_canvas_export_options_default(void)
{
  dt_canvas_export_options_t options;
  memset(&options, 0, sizeof(options));
  options.format = DT_CANVAS_EXPORT_PDF;
  options.dpi = 300.0f;
  options.quality = 92;
  options.icc_type = DT_COLORSPACE_SRGB;
  options.icc_filename[0] = '\0';
  options.intent = DT_INTENT_PERCEPTUAL;
  return options;
}

gboolean dt_canvas_export_format_carries_alpha(const dt_canvas_export_format_t format)
{
  return format != DT_CANVAS_EXPORT_JPEG;
}

const char *dt_canvas_export_extension(const dt_canvas_export_format_t format)
{
  switch(format)
  {
    case DT_CANVAS_EXPORT_PNG:
      return ".png";
    case DT_CANVAS_EXPORT_JPEG:
      return ".jpg";
    case DT_CANVAS_EXPORT_TIFF:
      return ".tif";
    default:
      return ".pdf";
  }
}

/** `book.png` and page 3 of 12 make `book_03.png`; a lone page keeps the name it was given. */
static gchar *_page_path(const char *path, const guint page, const guint pages)
{
  if(pages <= 1) return g_strdup(path);
  gchar *directory = g_path_get_dirname(path);
  gchar *base = g_path_get_basename(path);
  gchar *dot = strrchr(base, '.');
  gchar *extension = g_strdup(IS_NULL_PTR(dot) ? "" : dot);
  if(!IS_NULL_PTR(dot)) *dot = '\0';
  const int digits = pages >= 100 ? 3 : 2;
  gchar *name = g_strdup_printf("%s_%0*u%s", base, digits, page + 1, extension);
  gchar *full = g_build_filename(directory, name, NULL);
  dt_free(name);
  dt_free(extension);
  dt_free(base);
  dt_free(directory);
  return full;
}

/**
 * The pages the canvas asks for. A canvas divided into pages gives one per page holding a
 * frame, empty ones skipped; a canvas without gives one page around every frame. The page
 * size is the canvas's -- one canvas unit is one point -- never the exporter's.
 */
static GArray *_pages_of(const dt_canvas_t *canvas, const double bleed_pt)
{
  GArray *pages = g_array_new(FALSE, FALSE, sizeof(dt_canvas_export_page_t));
  double paper_width = 0.0;
  double paper_height = 0.0;
  if(dt_canvas_paper_dimensions(canvas, &paper_width, &paper_height))
  {
    const dt_canvas_rect_t bounds = dt_canvas_bounds(canvas);
    if(bounds.width > 0.0 && bounds.height > 0.0)
    {
      const int first_col = (int)floor(bounds.x / paper_width);
      const int last_col = (int)floor((bounds.x + bounds.width - 1e-9) / paper_width);
      const int first_row = (int)floor(bounds.y / paper_height);
      const int last_row = (int)floor((bounds.y + bounds.height - 1e-9) / paper_height);
      for(int row = first_row; row <= last_row; row++)
      {
        for(int col = first_col; col <= last_col; col++)
        {
          const dt_canvas_rect_t page_rect = dt_canvas_page_rect(canvas, col, row);
          gboolean holds_a_frame = FALSE;
          for(guint idx = 0; idx < dt_canvas_object_count(canvas) && !holds_a_frame; idx++)
          {
            const dt_canvas_object_t *object = dt_canvas_object_at(canvas, idx);
            if(!dt_canvas_object_is_frame(object) || (object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
            const dt_canvas_rect_t frame = dt_canvas_object_bounds(object);
            holds_a_frame = frame.x < page_rect.x + page_rect.width && frame.x + frame.width > page_rect.x
                            && frame.y < page_rect.y + page_rect.height && frame.y + frame.height > page_rect.y;
          }
          if(!holds_a_frame) continue;
          dt_canvas_export_page_t page;
          page.area = page_rect;
          g_array_append_val(pages, page);
        }
      }
    }
    if(pages->len == 0)
    {
      dt_canvas_export_page_t page;
      page.area = dt_canvas_page_rect(canvas, 0, 0);
      g_array_append_val(pages, page);
    }
  }
  else
  {
    dt_canvas_export_page_t page;
    page.area = dt_canvas_bounds(canvas);
    if(!(page.area.width > 0.0) || !(page.area.height > 0.0))
    {
      // Nothing on it: an A4's worth of background, so the file is still a page.
      page.area.x = -297.5;
      page.area.y = -421.0;
      page.area.width = 595.0;
      page.area.height = 842.0;
    }
    g_array_append_val(pages, page);
  }
  // The bleed grows every sheet, and the rectangle with it: what is out there gets drawn.
  for(guint idx = 0; idx < pages->len; idx++)
  {
    dt_canvas_export_page_t *page = &g_array_index(pages, dt_canvas_export_page_t, idx);
    page->area.x -= bleed_pt;
    page->area.y -= bleed_pt;
    page->area.width += 2.0 * bleed_pt;
    page->area.height += 2.0 * bleed_pt;
    page->width_pt = page->area.width;
    page->height_pt = page->area.height;
  }
  return pages;
}

/* --- colour ---------------------------------------------------------------------------- */

/** The profile as ICC bytes, NULL when it has none. The caller frees. */
static uint8_t *_profile_bytes(const dt_colorspaces_color_profile_t *profile, uint32_t *length)
{
  *length = 0;
  if(IS_NULL_PTR(profile)) return NULL;
  uint8_t *bytes = NULL;
  dt_colorspaces_lock_profile(profile);
  if(!IS_NULL_PTR(profile->profile))
  {
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(profile->profile, NULL, &size);
    if(size > 0)
    {
      bytes = g_try_malloc(size);
      if(!IS_NULL_PTR(bytes) && cmsSaveProfileToMem(profile->profile, bytes, &size))
        *length = size;
      else
      {
        dt_free(bytes);
        bytes = NULL;
      }
    }
  }
  dt_colorspaces_unlock_profile(profile);
  return bytes;
}

/**
 * Convert a cairo page (BGRx in memory) to packed RGB8, or to straight RGBA8 when `alpha`, in
 * the output profile. A cairo ARGB32 page is PREMULTIPLIED, so the colour is divided back out
 * before it is converted: a profile transform is not linear in coverage, and running it on a
 * premultiplied value darkens every edge towards black.
 */
static uint8_t *_page_to_output(const uint8_t *bgra, const int width, const int height, const int stride,
                                const dt_colorspaces_color_profile_t *output, const dt_iop_color_intent_t intent,
                                const gboolean alpha)
{
  const int channels = alpha ? 4 : 3;
  uint8_t *rgb = g_try_malloc((size_t)width * height * channels);
  if(IS_NULL_PTR(rgb)) return NULL;

  cmsHTRANSFORM transform = NULL;
  // The painter's page is in the canvas's own encoding, Adobe RGB (1998).
  const dt_colorspaces_color_profile_t *source = dt_colorspaces_get_profile(DT_COLORSPACE_ADOBERGB, "", DT_PROFILE_ROLE_OUTPUT);
  if(!IS_NULL_PTR(output) && !IS_NULL_PTR(source) && output != source)
  {
    // The transform does not retain the profiles, so the locks span its creation only.
    dt_colorspaces_lock_profile(source);
    dt_colorspaces_lock_profile(output);
    if(!IS_NULL_PTR(source->profile) && !IS_NULL_PTR(output->profile))
      transform = cmsCreateTransform(source->profile, TYPE_BGRA_8, output->profile,
                                     alpha ? TYPE_RGBA_8 : TYPE_RGB_8, (cmsUInt32Number)intent, 0);
    dt_colorspaces_unlock_profile(output);
    dt_colorspaces_unlock_profile(source);
  }

  uint8_t *straight = alpha ? g_try_malloc((size_t)width * 4) : NULL;
  if(alpha && IS_NULL_PTR(straight))
  {
    if(!IS_NULL_PTR(transform)) cmsDeleteTransform(transform);
    dt_free(rgb);
    return NULL;
  }
  for(int y = 0; y < height; y++)
  {
    const uint8_t *row = bgra + (size_t)y * stride;
    uint8_t *out = rgb + (size_t)y * width * channels;
    if(alpha)
    {
      // Back to straight colour, in cairo's own byte order, for the transform to read.
      for(int x = 0; x < width; x++)
      {
        const uint32_t coverage = row[4 * x + 3];
        for(int channel = 0; channel < 3; channel++)
          straight[4 * x + channel]
              = coverage == 0 ? 0 : (uint8_t)MIN(255u, (row[4 * x + channel] * 255u + coverage / 2) / coverage);
        straight[4 * x + 3] = (uint8_t)coverage;
      }
      row = straight;
    }
    if(!IS_NULL_PTR(transform))
    {
      cmsDoTransform(transform, row, out, width);
    }
    else
    {
      for(int x = 0; x < width; x++)
      {
        out[channels * x + 0] = row[4 * x + 2];
        out[channels * x + 1] = row[4 * x + 1];
        out[channels * x + 2] = row[4 * x + 0];
      }
    }
    // LCMS was asked for three channels; the coverage is copied across afterwards.
    if(alpha)
      for(int x = 0; x < width; x++) out[4 * x + 3] = row[4 * x + 3];
  }
  dt_free(straight);
  if(!IS_NULL_PTR(transform)) cmsDeleteTransform(transform);
  return rgb;
}

/**
 * Rasterise one page: `width` x `height` pixels covering `area` exactly, which is DPI times
 * the sheet's physical size and not one pixel more.
 */
static uint8_t *_render_page(const dt_canvas_t *canvas, const dt_canvas_rect_t *area, const int width,
                             const int height, const double scale,
                             const dt_colorspaces_color_profile_t *output, const dt_iop_color_intent_t intent,
                             dt_canvas_surface_cache_t *cache, const gboolean alpha)
{
  cairo_surface_t *page = cairo_image_surface_create(alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, width, height);
  if(cairo_surface_status(page) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(page);
    return NULL;
  }
  cairo_t *cr = cairo_create(page);
  dt_canvas_paint_options_t paint = dt_canvas_paint_options_export(cache, 1.0 / scale, *area);
  cairo_scale(cr, scale, scale);
  cairo_translate(cr, -area->x, -area->y);
  dt_canvas_paint(cr, canvas, &paint);
  cairo_destroy(cr);
  cairo_surface_flush(page);
  uint8_t *rgb = _page_to_output(cairo_image_surface_get_data(page), width, height,
                                 cairo_image_surface_get_stride(page), output, intent, alpha);
  cairo_surface_destroy(page);
  return rgb;
}

/* --- the writers ------------------------------------------------------------------------ */

#define ICC_MARKER (JPEG_APP0 + 2)
#define ICC_OVERHEAD_LEN 14
#define MAX_BYTES_IN_MARKER 65533
#define MAX_DATA_BYTES_IN_MARKER (MAX_BYTES_IN_MARKER - ICC_OVERHEAD_LEN)

typedef struct dt_canvas_jpeg_error_t
{
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
} dt_canvas_jpeg_error_t;

static void _jpeg_error_exit(j_common_ptr cinfo)
{
  dt_canvas_jpeg_error_t *error = (dt_canvas_jpeg_error_t *)cinfo->err;
  longjmp(error->setjmp_buffer, 1);
}

static void _jpeg_write_icc(j_compress_ptr cinfo, const uint8_t *icc_data, uint32_t icc_length)
{
  int marker_index = 1;
  while(icc_length > 0)
  {
    uint32_t chunk = icc_length;
    if(chunk > MAX_DATA_BYTES_IN_MARKER) chunk = MAX_DATA_BYTES_IN_MARKER;
    icc_length -= chunk;
    const uint32_t remaining = icc_length / MAX_DATA_BYTES_IN_MARKER + (icc_length % MAX_DATA_BYTES_IN_MARKER ? 1 : 0);
    jpeg_write_m_header(cinfo, ICC_MARKER, chunk + ICC_OVERHEAD_LEN);
    const char identifier[12] = "ICC_PROFILE";
    for(int idx = 0; idx < 12; idx++) jpeg_write_m_byte(cinfo, (int)identifier[idx]);
    jpeg_write_m_byte(cinfo, marker_index);
    jpeg_write_m_byte(cinfo, marker_index + (int)remaining);
    for(uint32_t idx = 0; idx < chunk; idx++) jpeg_write_m_byte(cinfo, icc_data[idx]);
    icc_data += chunk;
    marker_index++;
  }
}

/** Packed RGB8 to JPEG bytes in memory, the profile in an APP2 marker. NULL on failure. */
static uint8_t *_encode_jpeg(const uint8_t *rgb, const int width, const int height, const int quality,
                             const uint8_t *icc, const uint32_t icc_length, const double dpi, size_t *size)
{
  dt_canvas_jpeg_error_t error;
  struct jpeg_compress_struct cinfo;
  unsigned char *buffer = NULL;
  unsigned long buffer_size = 0;

  cinfo.err = jpeg_std_error(&error.pub);
  error.pub.error_exit = _jpeg_error_exit;
  if(setjmp(error.setjmp_buffer))
  {
    jpeg_destroy_compress(&cinfo);
    if(!IS_NULL_PTR(buffer)) free(buffer);
    return NULL;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buffer, &buffer_size);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, CLAMP(quality, 1, 100), TRUE);
  // A page carries text and edges as pixels: keep the chroma full at print qualities.
  if(quality > 90) cinfo.comp_info[0].v_samp_factor = 1;
  if(quality > 92) cinfo.comp_info[0].h_samp_factor = 1;
  cinfo.optimize_coding = 1;
  cinfo.density_unit = 1; // dots per inch
  cinfo.X_density = (UINT16)CLAMP((int)lround(dpi), 1, 65535);
  cinfo.Y_density = cinfo.X_density;
  jpeg_start_compress(&cinfo, TRUE);
  if(!IS_NULL_PTR(icc) && icc_length > 0) _jpeg_write_icc(&cinfo, icc, icc_length);
  for(int row = 0; row < height; row++)
  {
    JSAMPROW pointer = (JSAMPROW)(rgb + (size_t)row * width * 3);
    jpeg_write_scanlines(&cinfo, &pointer, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  *size = buffer_size;
  return buffer;
}

static gboolean _write_jpeg(const char *path, const uint8_t *rgb, const int width, const int height,
                            const int quality, const uint8_t *icc, const uint32_t icc_length, const double dpi)
{
  size_t size = 0;
  uint8_t *jpeg = _encode_jpeg(rgb, width, height, quality, icc, icc_length, dpi, &size);
  if(IS_NULL_PTR(jpeg)) return FALSE;
  FILE *file = g_fopen(path, "wb");
  const gboolean ok = !IS_NULL_PTR(file) && fwrite(jpeg, 1, size, file) == size;
  if(!IS_NULL_PTR(file)) fclose(file);
  free(jpeg);
  return ok;
}

static gboolean _write_png(const char *path, const uint8_t *rgb, const int width, const int height,
                           const uint8_t *icc, const uint32_t icc_length, const double dpi, const gboolean alpha)
{
  FILE *file = g_fopen(path, "wb");
  if(IS_NULL_PTR(file)) return FALSE;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = IS_NULL_PTR(png) ? NULL : png_create_info_struct(png);
  if(IS_NULL_PTR(png) || IS_NULL_PTR(info) || setjmp(png_jmpbuf(png)))
  {
    if(!IS_NULL_PTR(png)) png_destroy_write_struct(&png, IS_NULL_PTR(info) ? NULL : &info);
    fclose(file);
    return FALSE;
  }
  png_init_io(png, file);
  png_set_compression_level(png, 5);
  png_set_IHDR(png, info, width, height, 8, alpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  // Metres per inch, since pHYs counts pixels per metre.
  const png_uint_32 per_metre = (png_uint_32)lround(dpi / 0.0254);
  png_set_pHYs(png, info, per_metre, per_metre, PNG_RESOLUTION_METER);
  if(!IS_NULL_PTR(icc) && icc_length > 0)
    png_set_iCCP(png, info, "ICC profile", PNG_COMPRESSION_TYPE_BASE, (png_const_bytep)icc, icc_length);
  png_write_info(png, info);
  const size_t png_stride = (size_t)width * (alpha ? 4 : 3);
  for(int row = 0; row < height; row++) png_write_row(png, (png_const_bytep)(rgb + (size_t)row * png_stride));
  png_write_end(png, NULL);
  png_destroy_write_struct(&png, &info);
  fclose(file);
  return TRUE;
}

/** One directory of a multi-page TIFF, appended to an already-open file. */
static gboolean _write_tiff_page(TIFF *tiff, const uint8_t *rgb, const int width, const int height,
                                 const uint8_t *icc, const uint32_t icc_length, const double dpi,
                                 const guint page, const guint pages, const gboolean alpha)
{
  TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, (uint32_t)width);
  TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, (uint32_t)height);
  TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, alpha ? 3 + 1 : 3);
  if(alpha)
  {
    // Straight, not associated: the colour was divided back out of its coverage.
    const uint16_t extra[1] = { EXTRASAMPLE_UNASSALPHA };
    TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, extra);
  }
  TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
  TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff, 0));
  TIFFSetField(tiff, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
  TIFFSetField(tiff, TIFFTAG_XRESOLUTION, (float)dpi);
  TIFFSetField(tiff, TIFFTAG_YRESOLUTION, (float)dpi);
  if(pages > 1)
  {
    TIFFSetField(tiff, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
    TIFFSetField(tiff, TIFFTAG_PAGENUMBER, (uint16_t)page, (uint16_t)pages);
  }
  if(!IS_NULL_PTR(icc) && icc_length > 0) TIFFSetField(tiff, TIFFTAG_ICCPROFILE, icc_length, icc);
  const size_t tiff_stride = (size_t)width * (alpha ? 4 : 3);
  for(int row = 0; row < height; row++)
  {
    if(TIFFWriteScanline(tiff, (void *)(rgb + (size_t)row * tiff_stride), (uint32_t)row, 0) < 0) return FALSE;
  }
  return TIFFWriteDirectory(tiff) != 0;
}

static int _embed_profile(dt_pdf_t *pdf, const uint8_t *icc, const uint32_t icc_length)
{
  if(IS_NULL_PTR(icc) || icc_length == 0) return 0;
  return dt_pdf_add_icc_from_data(pdf, icc, icc_length);
}

/* --- the export -------------------------------------------------------------------------- */

gboolean dt_canvas_export(const dt_canvas_t *canvas, const char *path, const dt_canvas_export_options_t *options,
                          GError **error)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(path) || IS_NULL_PTR(options))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "nothing to export");
    return FALSE;
  }
  // A transparent plane needs a format that can carry one: writing it as JPEG would fill
  // every hole with a colour nobody chose, so it is refused rather than guessed at.
  const gboolean alpha = dt_canvas_background_is_transparent(canvas->background_style);
  if(alpha && !dt_canvas_export_format_carries_alpha(options->format))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO,
                "this canvas is transparent, and %s has no alpha channel to carry it",
                dt_canvas_export_extension(options->format) + 1);
    return FALSE;
  }
  const double dpi = options->dpi > 0.0f ? options->dpi : 300.0;
  // The bleed belongs to the document, beside the page size it grows: one canvas unit is one point.
  const double bleed_pt = fmax(canvas->page_bleed, 0.0f);
  const double scale = dpi / 72.0;
  GArray *pages = _pages_of(canvas, bleed_pt);

  const dt_colorspaces_color_profile_t *output = NULL;
  if(options->icc_type != DT_COLORSPACE_NONE)
    output = dt_colorspaces_get_profile(options->icc_type, options->icc_filename, DT_PROFILE_ROLE_OUTPUT);
  if(IS_NULL_PTR(output)) output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_ROLE_OUTPUT);
  uint32_t icc_length = 0;
  uint8_t *icc = _profile_bytes(output, &icc_length);

  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, EXPORT_SURFACE_CACHE_BYTES);
  dt_pdf_t *pdf = NULL;
  TIFF *tiff = NULL;
  dt_pdf_image_t **images = NULL;
  dt_pdf_page_t **pdf_pages = NULL;
  int icc_id = 0;
  gboolean ok = TRUE;

  if(options->format == DT_CANVAS_EXPORT_PDF)
  {
    const dt_canvas_export_page_t *first = &g_array_index(pages, dt_canvas_export_page_t, 0);
    pdf = dt_pdf_start(path, (float)first->width_pt, (float)first->height_pt, (float)dpi,
                       DT_PDF_STREAM_ENCODER_FLATE);
    if(IS_NULL_PTR(pdf)) ok = FALSE;
    else
    {
      icc_id = _embed_profile(pdf, icc, icc_length);
      images = g_new0(dt_pdf_image_t *, pages->len);
      pdf_pages = g_new0(dt_pdf_page_t *, pages->len);
    }
  }
  else if(options->format == DT_CANVAS_EXPORT_TIFF)
  {
    tiff = TIFFOpen(path, "w");
    if(IS_NULL_PTR(tiff)) ok = FALSE;
  }
  if(!ok)
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot write `%s'", path);
  }

  for(guint idx = 0; idx < pages->len && ok; idx++)
  {
    const dt_canvas_export_page_t *page = &g_array_index(pages, dt_canvas_export_page_t, idx);
    // The raster is the sheet's physical size times the resolution, and nothing more.
    const int width = (int)lround(page->width_pt / 72.0 * dpi);
    const int height = (int)lround(page->height_pt / 72.0 * dpi);
    if(width <= 0 || height <= 0 || (double)width * (double)height > EXPORT_MAX_PIXELS)
    {
      g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "page of %dx%d pixels is out of range", width, height);
      ok = FALSE;
      break;
    }
    uint8_t *rgb = _render_page(canvas, &page->area, width, height, scale, output, options->intent, cache, alpha);
    if(IS_NULL_PTR(rgb))
    {
      g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot render a page of `%s'", path);
      ok = FALSE;
      break;
    }
    if(options->format == DT_CANVAS_EXPORT_PDF)
    {
      // A page of photographs weighs what a photograph weighs: the stream is a JPEG unless
      // the user asked for every code back, in which case it stays a lossless flate one.
      // A transparent page is written as its colour plus a soft mask of its coverage, which
      // is how a PDF carries one; a lossy stream would blur the mask's own edges, so such a
      // page stays flate whatever the quality asked for.
      uint8_t *opaque = rgb;
      uint8_t *coverage = NULL;
      if(alpha)
      {
        opaque = g_try_malloc((size_t)width * height * 3);
        coverage = g_try_malloc((size_t)width * height);
        if(!IS_NULL_PTR(opaque) && !IS_NULL_PTR(coverage))
        {
          for(size_t pixel = 0; pixel < (size_t)width * height; pixel++)
          {
            for(int channel = 0; channel < 3; channel++) opaque[3 * pixel + channel] = rgb[4 * pixel + channel];
            coverage[pixel] = rgb[4 * pixel + 3];
          }
        }
      }
      const int mask_id = IS_NULL_PTR(coverage) ? 0 : dt_pdf_add_soft_mask(pdf, coverage, width, height);
      if(IS_NULL_PTR(opaque) || (alpha && mask_id <= 0))
      {
        ok = FALSE;
      }
      else if(options->quality >= 100 || alpha)
      {
        images[idx] = dt_pdf_add_image_masked(pdf, opaque, width, height, 8, icc_id, mask_id, 0.0f);
      }
      else
      {
        size_t jpeg_size = 0;
        uint8_t *jpeg = _encode_jpeg(opaque, width, height, options->quality, NULL, 0, dpi, &jpeg_size);
        if(!IS_NULL_PTR(jpeg))
        {
          images[idx] = dt_pdf_add_image_jpeg(pdf, jpeg, jpeg_size, width, height, icc_id, 0.0f);
          free(jpeg);
        }
      }
      if(alpha) dt_free(opaque);
      dt_free(coverage);
      if(IS_NULL_PTR(images[idx]))
      {
        ok = FALSE;
      }
      else
      {
        images[idx]->bb_x = 0.0f;
        images[idx]->bb_y = 0.0f;
        images[idx]->bb_width = (float)page->width_pt;
        images[idx]->bb_height = (float)page->height_pt;
        pdf_pages[idx] = dt_pdf_add_page(pdf, &images[idx], 1);
      }
    }
    else if(options->format == DT_CANVAS_EXPORT_TIFF)
    {
      ok = _write_tiff_page(tiff, rgb, width, height, icc, icc_length, dpi, idx, pages->len, alpha);
    }
    else
    {
      gchar *page_path = _page_path(path, idx, pages->len);
      ok = options->format == DT_CANVAS_EXPORT_PNG
               ? _write_png(page_path, rgb, width, height, icc, icc_length, dpi, alpha)
               : _write_jpeg(page_path, rgb, width, height, options->quality, icc, icc_length, dpi);
      if(!ok) g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot write `%s'", page_path);
      dt_free(page_path);
    }
    dt_free(rgb);
  }
  dt_canvas_surface_cache_free(cache);

  if(!IS_NULL_PTR(pdf))
  {
    dt_pdf_finish(pdf, ok ? pdf_pages : NULL, ok ? (int)pages->len : 0);
    for(guint idx = 0; idx < pages->len; idx++)
    {
      dt_free(images[idx]);
      dt_free(pdf_pages[idx]);
    }
    dt_free(images);
    dt_free(pdf_pages);
  }
  if(!IS_NULL_PTR(tiff)) TIFFClose(tiff);
  dt_free(icc);
  g_array_free(pages, TRUE);
  if(!ok && IS_NULL_PTR(*error))
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot write `%s'", path);
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
