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
  // The painter's page is in the canvas's own encoding, Adobe RGB (1998).
  const dt_colorspaces_color_profile_t *source = dt_colorspaces_get_profile(DT_COLORSPACE_ADOBERGB, "", DT_PROFILE_ROLE_OUTPUT);
  if(!IS_NULL_PTR(output) && !IS_NULL_PTR(source) && output != source)
  {
    // The transform does not retain the profiles, so the locks span its creation only.
    dt_colorspaces_lock_profile(source);
    dt_colorspaces_lock_profile(output);
    if(!IS_NULL_PTR(source->profile) && !IS_NULL_PTR(output->profile))
      transform = cmsCreateTransform(source->profile, TYPE_BGRA_8, output->profile, TYPE_RGB_8, (cmsUInt32Number)intent, 0);
    dt_colorspaces_unlock_profile(output);
    dt_colorspaces_unlock_profile(source);
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

/** Rasterise one canvas rectangle onto a page of `width` x `height` pixels, in the output profile. */
static uint8_t *_render_page(const dt_canvas_t *canvas, const dt_canvas_rect_t *area, const int width, const int height,
                             const double scale, const double offset_x, const double offset_y,
                             const dt_colorspaces_color_profile_t *output, const dt_iop_color_intent_t intent,
                             dt_canvas_surface_cache_t *cache)
{
  cairo_surface_t *page = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  if(cairo_surface_status(page) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(page);
    return NULL;
  }
  cairo_t *cr = cairo_create(page);
  dt_canvas_rect_t whole_page;
  whole_page.x = area->x - offset_x / scale;
  whole_page.y = area->y - offset_y / scale;
  whole_page.width = width / scale;
  whole_page.height = height / scale;
  dt_canvas_paint_options_t paint = dt_canvas_paint_options_export(cache, 1.0 / scale, whole_page);
  cairo_translate(cr, offset_x, offset_y);
  cairo_scale(cr, scale, scale);
  cairo_translate(cr, -area->x, -area->y);
  dt_canvas_paint(cr, canvas, &paint);
  cairo_destroy(cr);
  cairo_surface_flush(page);
  uint8_t *rgb = _page_to_output(cairo_image_surface_get_data(page), width, height,
                                 cairo_image_surface_get_stride(page), output, intent);
  cairo_surface_destroy(page);
  return rgb;
}

typedef struct dt_canvas_pdf_page_t
{
  dt_canvas_rect_t area;    ///< the canvas rectangle on this page
  double scale;             ///< pixels per canvas unit
  double offset_x;          ///< where the area lands on the page, pixels
  double offset_y;
} dt_canvas_pdf_page_t;

gboolean dt_canvas_pdf_export(const dt_canvas_t *canvas, const char *path, const dt_canvas_pdf_options_t *options,
                              GError **error)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(path) || IS_NULL_PTR(options))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "nothing to export");
    return FALSE;
  }
  const float dpi = options->dpi > 0.0f ? options->dpi : 300.0f;

  // The pages: the canvas's own paper, one PDF page per canvas page holding a frame; or,
  // without paper, the frames' box fitted inside the option's page.
  double paper_width = 0.0;
  double paper_height = 0.0;
  GArray *pages = g_array_new(FALSE, FALSE, sizeof(dt_canvas_pdf_page_t));
  double page_width_mm = options->page_width_mm;
  double page_height_mm = options->page_height_mm;
  if(dt_canvas_paper_dimensions(canvas, &paper_width, &paper_height))
  {
    // One canvas unit is one point.
    page_width_mm = dt_pdf_point_to_mm(paper_width);
    page_height_mm = dt_pdf_point_to_mm(paper_height);
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
          dt_canvas_pdf_page_t page;
          page.area = page_rect;
          page.scale = dpi / 72.0;
          page.offset_x = 0.0;
          page.offset_y = 0.0;
          g_array_append_val(pages, page);
        }
      }
    }
    if(pages->len == 0)
    {
      dt_canvas_pdf_page_t page;
      page.area = dt_canvas_page_rect(canvas, 0, 0);
      page.scale = dpi / 72.0;
      page.offset_x = 0.0;
      page.offset_y = 0.0;
      g_array_append_val(pages, page);
    }
  }
  else
  {
    const double page_width_px = page_width_mm / 25.4 * dpi;
    const double page_height_px = page_height_mm / 25.4 * dpi;
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
    dt_canvas_pdf_page_t page;
    page.area = bounds;
    page.scale = scale;
    page.offset_x = margin_px + (inner_width - bounds.width * scale) * 0.5;
    page.offset_y = margin_px + (inner_height - bounds.height * scale) * 0.5;
    g_array_append_val(pages, page);
  }

  const int width = (int)lround(page_width_mm / 25.4 * dpi);
  const int height = (int)lround(page_height_mm / 25.4 * dpi);
  if(width <= 0 || height <= 0 || (double)width * (double)height > PDF_MAX_PIXELS)
  {
    g_array_free(pages, TRUE);
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "page of %dx%d pixels is out of range", width, height);
    return FALSE;
  }

  const dt_colorspaces_color_profile_t *output = NULL;
  if(options->icc_type != DT_COLORSPACE_NONE)
    output = dt_colorspaces_get_profile(options->icc_type, options->icc_filename, DT_PROFILE_ROLE_OUTPUT);
  if(IS_NULL_PTR(output)) output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_ROLE_OUTPUT);

  const float page_width_pt = dt_pdf_mm_to_point(page_width_mm);
  const float page_height_pt = dt_pdf_mm_to_point(page_height_mm);
  dt_pdf_t *pdf = dt_pdf_start(path, page_width_pt, page_height_pt, dpi, DT_PDF_STREAM_ENCODER_FLATE);
  if(IS_NULL_PTR(pdf))
  {
    g_array_free(pages, TRUE);
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot write `%s'", path);
    return FALSE;
  }
  const int icc_id = _embed_profile(pdf, output);
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, PDF_SURFACE_CACHE_BYTES);
  dt_pdf_image_t **images = g_new0(dt_pdf_image_t *, pages->len);
  dt_pdf_page_t **pdf_pages = g_new0(dt_pdf_page_t *, pages->len);
  gboolean ok = TRUE;
  for(guint idx = 0; idx < pages->len && ok; idx++)
  {
    const dt_canvas_pdf_page_t *page = &g_array_index(pages, dt_canvas_pdf_page_t, idx);
    uint8_t *rgb = _render_page(canvas, &page->area, width, height, page->scale, page->offset_x, page->offset_y,
                                output, options->intent, cache);
    if(IS_NULL_PTR(rgb))
    {
      ok = FALSE;
      break;
    }
    images[idx] = dt_pdf_add_image(pdf, rgb, width, height, 8, icc_id, 0.0f);
    dt_free(rgb);
    if(IS_NULL_PTR(images[idx]))
    {
      ok = FALSE;
      break;
    }
    images[idx]->bb_x = 0.0f;
    images[idx]->bb_y = 0.0f;
    images[idx]->bb_width = page_width_pt;
    images[idx]->bb_height = page_height_pt;
    pdf_pages[idx] = dt_pdf_add_page(pdf, &images[idx], 1);
  }
  dt_canvas_surface_cache_free(cache);
  if(ok)
  {
    dt_pdf_finish(pdf, pdf_pages, (int)pages->len);
  }
  else
  {
    dt_pdf_finish(pdf, NULL, 0);
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot render a page of `%s'", path);
  }
  for(guint idx = 0; idx < pages->len; idx++)
  {
    dt_free(images[idx]);
    dt_free(pdf_pages[idx]);
  }
  dt_free(images);
  dt_free(pdf_pages);
  g_array_free(pages, TRUE);
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
