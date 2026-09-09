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

#include "canvas/canvas_pdf.h"

#include "canvas/canvas_format.h"
#include "canvas/canvas_paint.h"
#include "canvas/canvas_render.h"
#include "colorprofiles/colorspaces.h"
#include "common/pdf.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <cairo.h>
#include <lcms2.h>
#include <math.h>
#include <string.h>

/** More than this and the raster is a mistake, not a print. */
#define PDF_MAX_PIXELS (200u * 1024u * 1024u)
#define PDF_SURFACE_CACHE_BYTES (512u * 1024u * 1024u)

dt_canvas_pdf_options_t dt_canvas_pdf_options_default(void)
{
  dt_canvas_pdf_options_t options;
  memset(&options, 0, sizeof(options));
  options.page_width_mm = 210.0f;
  options.page_height_mm = 297.0f;
  options.margin_mm = 10.0f;
  options.dpi = 300.0f;
  options.icc_type = DT_COLORSPACE_SRGB;
  options.icc_filename[0] = '\0';
  options.intent = DT_INTENT_PERCEPTUAL;
  return options;
}

/** Convert a cairo RGB24 page (BGRx in memory) to packed RGB8 in the output profile. */
static uint8_t *_page_to_output(const uint8_t *bgra, const int width, const int height, const int stride,
                                const dt_colorspaces_color_profile_t *output, const dt_iop_color_intent_t intent)
{
  uint8_t *rgb = g_try_malloc((size_t)width * height * 3);
  if(IS_NULL_PTR(rgb)) return NULL;

  cmsHTRANSFORM transform = NULL;
  const dt_colorspaces_color_profile_t *srgb = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_ROLE_OUTPUT);
  if(!IS_NULL_PTR(output) && !IS_NULL_PTR(srgb) && output != srgb)
  {
    // The transform does not retain the profiles, so the locks span its creation only.
    dt_colorspaces_lock_profile(srgb);
    dt_colorspaces_lock_profile(output);
    if(!IS_NULL_PTR(srgb->profile) && !IS_NULL_PTR(output->profile))
      transform = cmsCreateTransform(srgb->profile, TYPE_BGRA_8, output->profile, TYPE_RGB_8, (cmsUInt32Number)intent, 0);
    dt_colorspaces_unlock_profile(output);
    dt_colorspaces_unlock_profile(srgb);
  }

  for(int y = 0; y < height; y++)
  {
    const uint8_t *row = bgra + (size_t)y * stride;
    uint8_t *out = rgb + (size_t)y * width * 3;
    if(!IS_NULL_PTR(transform))
    {
      cmsDoTransform(transform, row, out, width);
    }
    else
    {
      for(int x = 0; x < width; x++)
      {
        out[3 * x + 0] = row[4 * x + 2];
        out[3 * x + 1] = row[4 * x + 1];
        out[3 * x + 2] = row[4 * x + 0];
      }
    }
  }
  if(!IS_NULL_PTR(transform)) cmsDeleteTransform(transform);
  return rgb;
}

static int _embed_profile(dt_pdf_t *pdf, const dt_colorspaces_color_profile_t *profile)
{
  if(IS_NULL_PTR(profile)) return 0;
  int icc_id = 0;
  dt_colorspaces_lock_profile(profile);
  if(!IS_NULL_PTR(profile->profile))
  {
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(profile->profile, NULL, &size);
    if(size > 0)
    {
      uint8_t *bytes = g_try_malloc(size);
      if(!IS_NULL_PTR(bytes) && cmsSaveProfileToMem(profile->profile, bytes, &size))
        icc_id = dt_pdf_add_icc_from_data(pdf, bytes, size);
      dt_free(bytes);
    }
  }
  dt_colorspaces_unlock_profile(profile);
  return icc_id;
}

gboolean dt_canvas_pdf_export(const dt_canvas_t *canvas, const char *path, const dt_canvas_pdf_options_t *options,
                              GError **error)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(path) || IS_NULL_PTR(options))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "nothing to export");
    return FALSE;
  }
  const float dpi = options->dpi > 0.0f ? options->dpi : 300.0f;
  const double page_width_px = options->page_width_mm / 25.4 * dpi;
  const double page_height_px = options->page_height_mm / 25.4 * dpi;
  const int width = (int)lround(page_width_px);
  const int height = (int)lround(page_height_px);
  if(width <= 0 || height <= 0 || (double)width * (double)height > PDF_MAX_PIXELS)
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "page of %dx%d pixels is out of range", width, height);
    return FALSE;
  }

  cairo_surface_t *page = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  if(cairo_surface_status(page) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(page);
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot allocate a %dx%d page", width, height);
    return FALSE;
  }
  cairo_t *cr = cairo_create(page);

  // Fit the frames' box inside the margins, centred.
  const double margin_px = options->margin_mm / 25.4 * dpi;
  const double inner_width = fmax(page_width_px - 2.0 * margin_px, 1.0);
  const double inner_height = fmax(page_height_px - 2.0 * margin_px, 1.0);
  dt_canvas_rect_t bounds = dt_canvas_bounds(canvas);
  double scale = 1.0;
  if(bounds.width > 0.0 && bounds.height > 0.0)
  {
    scale = fmin(inner_width / bounds.width, inner_height / bounds.height);
  }
  else
  {
    bounds.x = -inner_width * 0.5;
    bounds.y = -inner_height * 0.5;
    bounds.width = inner_width;
    bounds.height = inner_height;
  }
  const double drawn_width = bounds.width * scale;
  const double drawn_height = bounds.height * scale;
  const double offset_x = margin_px + (inner_width - drawn_width) * 0.5;
  const double offset_y = margin_px + (inner_height - drawn_height) * 0.5;

  // The background covers the whole page, frames land inside the margins.
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, PDF_SURFACE_CACHE_BYTES);
  dt_canvas_rect_t whole_page;
  whole_page.x = -offset_x / scale + bounds.x;
  whole_page.y = -offset_y / scale + bounds.y;
  whole_page.width = page_width_px / scale;
  whole_page.height = page_height_px / scale;
  dt_canvas_paint_options_t paint = dt_canvas_paint_options_export(cache, 1.0 / scale, whole_page);
  cairo_translate(cr, offset_x, offset_y);
  cairo_scale(cr, scale, scale);
  cairo_translate(cr, -bounds.x, -bounds.y);
  dt_canvas_paint(cr, canvas, &paint);
  cairo_destroy(cr);
  dt_canvas_surface_cache_free(cache);
  cairo_surface_flush(page);

  const dt_colorspaces_color_profile_t *output = NULL;
  if(options->icc_type != DT_COLORSPACE_NONE)
    output = dt_colorspaces_get_profile(options->icc_type, options->icc_filename, DT_PROFILE_ROLE_OUTPUT);
  if(IS_NULL_PTR(output)) output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_ROLE_OUTPUT);

  uint8_t *rgb = _page_to_output(cairo_image_surface_get_data(page), width, height,
                                 cairo_image_surface_get_stride(page), output, options->intent);
  cairo_surface_destroy(page);
  if(IS_NULL_PTR(rgb))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot convert the page to the output profile");
    return FALSE;
  }

  const float page_width_pt = dt_pdf_mm_to_point(options->page_width_mm);
  const float page_height_pt = dt_pdf_mm_to_point(options->page_height_mm);
  dt_pdf_t *pdf = dt_pdf_start(path, page_width_pt, page_height_pt, dpi, DT_PDF_STREAM_ENCODER_FLATE);
  if(IS_NULL_PTR(pdf))
  {
    dt_free(rgb);
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot write `%s'", path);
    return FALSE;
  }
  const int icc_id = _embed_profile(pdf, output);
  dt_pdf_image_t *image = dt_pdf_add_image(pdf, rgb, width, height, 8, icc_id, 0.0f);
  dt_free(rgb);
  if(IS_NULL_PTR(image))
  {
    dt_pdf_finish(pdf, NULL, 0);
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot add the page image to `%s'", path);
    return FALSE;
  }
  image->bb_x = 0.0f;
  image->bb_y = 0.0f;
  image->bb_width = page_width_pt;
  image->bb_height = page_height_pt;
  dt_pdf_page_t *pdf_page = dt_pdf_add_page(pdf, &image, 1);
  dt_pdf_finish(pdf, &pdf_page, 1);
  dt_free(pdf_page);
  dt_free(image);
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
