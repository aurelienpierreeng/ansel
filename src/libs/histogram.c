/*
    This file is part of Ansel,
    Copyright (C) 2024 - Aurélien Pierre.

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

#include <stdint.h>

#include "bauhaus/bauhaus.h"
#include "common/atomic.h"
#include "common/color_vocabulary.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/drawingarea.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "libs/colorpicker.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#define HISTOGRAM_BINS 256
#define TONES 128
#define GAMMA 1.f / 1.5f

DT_MODULE(1)

typedef enum dt_lib_histogram_scope_type_t
{
  DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM = 0,
  DT_LIB_HISTOGRAM_SCOPE_WAVEFORM_HORIZONTAL,
  DT_LIB_HISTOGRAM_SCOPE_WAVEFORM_VERTICAL,
  DT_LIB_HISTOGRAM_SCOPE_PARADE_HORIZONTAL,
  DT_LIB_HISTOGRAM_SCOPE_PARADE_VERTICAL,
  DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE,
  DT_LIB_HISTOGRAM_SCOPE_N // needs to be the last one
} dt_lib_histogram_scope_type_t;


typedef struct dt_lib_histogram_cache_t
{
  // If any of those params changes, we need to recompute the Cairo buffer.
  float zoom;
  int width;
  int height;
  uint64_t hash;
  dt_lib_histogram_scope_type_t view;
} dt_lib_histogram_cache_t;

typedef struct dt_lib_histogram_t
{
  GtkWidget *scope_draw;               // GtkDrawingArea -- scope, scale, and draggable overlays
  GtkWidget *stage;                    // Module at which stage we sample histogram
  GtkWidget *display;                  // Kind of display
  dt_backbuf_t *backbuf;               // reference to the dev backbuf currently in use
  const char *op;
  float zoom; // zoom level for the vectorscope

  dt_lib_histogram_cache_t cache;
  cairo_surface_t *cst;
} dt_lib_histogram_t;

const char *name(dt_lib_module_t *self)
{
  return _("scopes");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 1;
}

int position()
{
  return 1000;
}


dt_backbuf_t * _get_backuf(dt_develop_t *dev, const char *op)
{
  if(!strcmp(op, "demosaic"))
    return &dev->raw_histogram;
  else if(!strcmp(op, "colorout"))
    return &dev->output_histogram;
  else if(!strcmp(op, "gamma"))
    return &dev->display_histogram;
  else
    return NULL;
}


void _backbuf_int_to_op(const int value, dt_lib_histogram_t *d)
{
  switch(value)
  {
    case 0:
    {
      d->op = "demosaic";
      break;
    }
    case 1:
    {
      d->op = "colorout";
      break;
    }
    case 2:
    {
      d->op = "gamma";
    }
  }
}

int _backbuf_op_to_int(dt_lib_histogram_t *d)
{
  if(!strcmp(d->op, "demosaic")) return 0;
  if(!strcmp(d->op, "colorout")) return 1;
  if(!strcmp(d->op, "gamma")) return 2;
  return 2;
}

void _reset_cache(dt_lib_histogram_t *d)
{
  d->cache.view = DT_LIB_HISTOGRAM_SCOPE_N;
  d->cache.width = -1;
  d->cache.height = -1;
  d->cache.hash = (uint64_t)-1;
  d->cache.zoom = -1.;
}


static gboolean _is_backbuf_ready(dt_lib_histogram_t *d)
{
  return (darktable.develop->preview_status == DT_DEV_PIXELPIPE_VALID) &&
         (d->backbuf->hash != (uint64_t)-1) &&
         (d->backbuf->buffer != NULL);
}

static void _redraw_scopes(dt_lib_histogram_t *d)
{
  gtk_widget_queue_draw(d->scope_draw);
}

static void _process_histogram(dt_backbuf_t *backbuf, cairo_t *cr, const int width, const int height)
{
  uint32_t *bins = calloc(4 * HISTOGRAM_BINS, sizeof(uint32_t));
  if(bins == NULL) return;

  // Compute stats - Do we need to restrict them to the color picker selection ? I don't see the problem it solves.
  dt_dev_histogram_collection_params_t histogram_params = { 0 };
  const dt_iop_colorspace_type_t cst = IOP_CS_RGB;
  dt_dev_histogram_stats_t histogram_stats = { .bins_count = HISTOGRAM_BINS, .ch = 4, .pixels = 0 };
  uint32_t histogram_max[4] = { 0 };
  dt_histogram_roi_t roi = { .width = backbuf->width,
                             .height = backbuf->height,
                             .crop_x = 0,
                             .crop_y = 0,
                             .crop_width = 0,
                             .crop_height = 0 };
  histogram_params.roi = &roi;
  histogram_params.bins_count = HISTOGRAM_BINS;
  histogram_params.mul = histogram_params.bins_count - 1;

  dt_histogram_helper(&histogram_params, &histogram_stats, cst, IOP_CS_NONE, backbuf->buffer, &bins, FALSE, NULL);
  dt_histogram_max_helper(&histogram_stats, cst, IOP_CS_NONE, &bins, histogram_max);
  uint32_t overall_histogram_max = MAX(MAX(histogram_max[0], histogram_max[1]), histogram_max[2]);

  // Draw thingy
  if(overall_histogram_max > 0)
  {
    // Paint background
    cairo_rectangle(cr, 0, 0, width, height);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill(cr);

    set_color(cr, darktable.bauhaus->graph_grid);
    dt_draw_grid(cr, 4, 0, 0, width, height);

    cairo_save(cr);
    cairo_push_group_with_content(cr, CAIRO_CONTENT_COLOR);
    cairo_translate(cr, 0, height);
    cairo_scale(cr, width / 255.0, - (double)height / (double)(1. + log(overall_histogram_max)));
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);

    for(int k = 0; k < 3; k++)
    {
      set_color(cr, darktable.bauhaus->graph_colors[k]);
      dt_draw_histogram_8(cr, bins, 4, k, FALSE);
    }

    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    cairo_paint_with_alpha(cr, 0.5);
    cairo_restore(cr);
  }

  free(bins);
}


uint32_t _find_max_histogram(const uint32_t *const restrict bins, const size_t binning_size)
{
  uint32_t max_hist = 0;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
        aligned(bins: 64) \
        dt_omp_firstprivate(bins, binning_size) \
        reduction(max: max_hist) \
        schedule(static)
#endif
  for(size_t k = 0; k < binning_size; k++) if(bins[k] > max_hist) max_hist = bins[k];

  return max_hist;
}


static inline void _bin_pixels_waveform(const float *const restrict image, uint32_t *const restrict bins,
                                        const size_t width, const size_t height, const size_t binning_size,
                                        const gboolean vertical)
{
  // Init
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
        aligned(bins: 64) \
        dt_omp_firstprivate(bins, binning_size) \
        schedule(static)
#endif
  for(size_t k = 0; k < binning_size; k++) bins[k] = 0;

  // Process
#ifdef _OPENMP
#ifndef _WIN32
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(image, height, width, binning_size, vertical) \
        reduction(+: bins[0: binning_size]) \
        schedule(static) collapse(3)
#else
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(image, height, width, binning_size, vertical) \
        shared(bins) \
        schedule(static) collapse(3)
#endif
#endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
      for(size_t c = 0; c < 3; c++)
      {
        const float value = image[(i * width + j) * 4 + c];
        const size_t index = (uint8_t)CLAMP(roundf(value * (TONES - 1)), 0, TONES - 1);
        if(vertical)
          bins[((i * (TONES)) + index) * 4 + c]++;
        else
          bins[(((TONES - 1) - index) * width + j) * 4 + c]++;
      }
}

static void _create_waveform_image(const uint32_t *const restrict bins, uint8_t *const restrict image,
                                   const uint32_t max_hist,
                                   const size_t width, const size_t height)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
        aligned(image, bins: 64) \
        dt_omp_firstprivate(image, height, width, bins, max_hist) \
        schedule(static)
#endif
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    image[k + 3] = 255; // alpha

    // We apply a slight "gamma" boost for legibility
    image[k + 2] = (uint8_t)CLAMP(roundf(powf((float)bins[k + 0] / (float)max_hist, GAMMA) * 255.f), 0, 255);
    image[k + 1] = (uint8_t)CLAMP(roundf(powf((float)bins[k + 1] / (float)max_hist, GAMMA) * 255.f), 0, 255);
    image[k + 0] = (uint8_t)CLAMP(roundf(powf((float)bins[k + 2] / (float)max_hist, GAMMA) * 255.f), 0, 255);
  }
}

static void _mask_waveform(const uint8_t *const restrict image, uint8_t *const restrict masked, const size_t width, const size_t height, const size_t channel)
{
  // Channel masking, aka extract the desired channel out of the RGBa image
  uint8_t mask[4] = { 0, 0, 0, 0 };
  for(size_t k = 0; k < 4; k++)
    if(k == channel) mask[k] = 1;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(image, masked, height, width, mask) \
        schedule(static)
#endif
    for(size_t i = 0; i < height; i++)
      for(size_t j = 0; j < width; j++)
      {
        const size_t index = (i * width + j) * 4;
        const uint8_t *const restrict pixel_in = image + index;
        uint8_t *const restrict pixel_out = masked + index;

#ifdef _OPENMP
#pragma omp simd aligned(mask, pixel_in, pixel_out: 16)
#endif
        for(size_t c = 0; c < 4; c++) pixel_out[c] = pixel_in[c] * mask[c];
      }
}

static void _paint_waveform(cairo_t *cr, uint8_t *const restrict image, const int width, const int height, const size_t img_width, const size_t img_height, const gboolean vertical)
{
  const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, img_width);
  cairo_surface_t *background = cairo_image_surface_create_for_data(image, CAIRO_FORMAT_ARGB32, img_width, img_height, stride);
  const double scale_w = (vertical) ? (double)width / (double)TONES
                                    : (double)width / (double)img_width;
  const double scale_h = (vertical) ? (double)height / (double)img_height
                                    : (double)height / (double)TONES;
  cairo_scale(cr, scale_w, scale_h);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_source_surface(cr, background, 0., 0.);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_paint(cr);
  cairo_surface_destroy(background);
}

static void _paint_parade(cairo_t *cr, uint8_t *const restrict image, const int width, const int height, const size_t img_width, const size_t img_height, const gboolean vertical)
{
  const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, img_width);
  const double scale_w = (vertical) ? (double)width / (double)TONES
                                    : (double)width / (double)img_width / 3.;
  const double scale_h = (vertical) ? (double)height / (double)img_height / 3.
                                    : (double)height / (double)TONES;
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_scale(cr, scale_w, scale_h);

  // The parade is basically a waveform where channels are shown
  // next to each other instead of on top of each other.
  // We need to isolate each channel, then paint it at a third of the nominal image width/height.
  for(int c = 0; c < 3; c++)
  {
    uint8_t *const restrict channel = dt_alloc_align(img_width * img_height * 4 * sizeof(uint8_t));
    _mask_waveform(image, channel, img_width, img_height, c);
    cairo_surface_t *background = cairo_image_surface_create_for_data(channel, CAIRO_FORMAT_ARGB32, img_width, img_height, stride);
    const double x = (vertical) ? 0. : (double)c * img_width;
    const double y = (vertical) ? (double)c * img_height : 0.;
    cairo_set_source_surface(cr, background, x, y);
    cairo_paint(cr);
    cairo_surface_destroy(background);
    dt_free_align(channel);
  }
}


static void _process_waveform(dt_backbuf_t *backbuf, cairo_t *cr, const int width, const int height, const gboolean vertical, const gboolean parade)
{
  const size_t binning_size = (vertical) ? 4 * TONES * backbuf->height : 4 * TONES * backbuf->width;

  // 1. Pixel binning along columns/rows, aka compute a column/row-wise histogram
  uint32_t *const restrict bins = dt_alloc_align(binning_size * sizeof(uint32_t));
  _bin_pixels_waveform(backbuf->buffer, bins, backbuf->width, backbuf->height, binning_size, vertical);

  // 2. Paint image.
  // In a 1D histogram, pixel frequencies are shown as height (y axis) for each RGB quantum (x axis).
  // Here, we do a sort of 2D histogram : pixel frequencies are shown as opacity ("z" axis),
  // for each image column (x axis), for each RGB quantum (y axis)
  uint8_t *const restrict image = dt_alloc_align(binning_size * sizeof(uint8_t));
  const size_t img_width = (vertical) ? TONES : backbuf->width;
  const size_t img_height = (vertical) ? backbuf->height : TONES;
  const uint32_t overall_max_hist = _find_max_histogram(bins, binning_size);
  _create_waveform_image(bins, image, overall_max_hist, img_width, img_height);
  dt_free_align(bins);

  // 3. Send everything to GUI buffer.
  if(overall_max_hist > 0)
  {
    cairo_save(cr);

    // Paint background - Color not exposed to user theme because this is tricky
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.21, 0.21, 0.21);
    dt_draw_grid(cr, 4, 0, 0, width, height);

    if(parade)
      _paint_parade(cr, image, width, height, img_width, img_height, vertical);
    else
      _paint_waveform(cr, image, width, height, img_width, img_height, vertical);

    cairo_restore(cr);
  }

  dt_free_align(image);
}

static float _Luv_to_vectorscope_coord_zoom(const float value, const float zoom)
{
  // Convert u, v coordinates of Luv vectors into x, y coordinates
  // into the space of the vectorscope square buffer
  return (value + zoom) * (HISTOGRAM_BINS - 1) / (2.f * zoom);
}

static float _vectorscope_coord_zoom_to_Luv(const float value, const float zoom)
{
  // Inverse of the above
  return value * (2.f * zoom) / (HISTOGRAM_BINS - 1) - zoom;
}

static void _bin_pixels_vectorscope(const float *const restrict image, uint32_t *const restrict vectorscope,
                                    dt_iop_order_iccprofile_info_t *profile,
                                    const size_t n_pixels, const float zoom)
{
  // Init
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
        aligned(vectorscope: 64) \
        dt_omp_firstprivate(vectorscope) \
        schedule(static)
#endif
  for(size_t k = 0; k < HISTOGRAM_BINS * HISTOGRAM_BINS; k++) vectorscope[k] = 0;

#ifdef _OPENMP
#ifndef _WIN32
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(image, n_pixels, profile, zoom) \
        reduction(+: vectorscope[0: HISTOGRAM_BINS * HISTOGRAM_BINS]) \
        schedule(static)
#else
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(image, n_pixels, profile, zoom) \
        shared(vectorscope) \
        schedule(static)
#endif
#endif
  for(size_t k = 0; k < n_pixels * 4; k += 4)
  {
    dt_aligned_pixel_t XYZ_D50 = { 0.f };
    dt_aligned_pixel_t xyY = { 0.f };
    dt_aligned_pixel_t Luv = { 0.f };
    dt_ioppr_rgb_matrix_to_xyz(image + k, XYZ_D50, profile->matrix_in_transposed, profile->lut_in, profile->unbounded_coeffs_in,
                               profile->lutsize, profile->nonlinearlut);
    dt_XYZ_to_xyY(XYZ_D50, xyY);
    dt_xyY_to_Luv(xyY, Luv);

    // Luv is sampled between 0 and 100.0f, u and v between +/- 220.f
    const size_t U_index = (size_t)CLAMP(roundf(_Luv_to_vectorscope_coord_zoom(Luv[1], zoom)), 0, HISTOGRAM_BINS - 1);
    const size_t V_index = (size_t)CLAMP(roundf(_Luv_to_vectorscope_coord_zoom(Luv[2], zoom)), 0, HISTOGRAM_BINS - 1);

    // We put V = 0 at the bottom of the image.
    vectorscope[(HISTOGRAM_BINS - 1 - V_index) * HISTOGRAM_BINS + U_index]++;
  }
}

static void _create_vectorscope_image(const uint32_t *const restrict vectorscope, uint8_t *const restrict image,
                                      dt_iop_order_iccprofile_info_t *profile,
                                      const float max_hist, const float zoom)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(image, vectorscope, profile, max_hist, zoom) \
        schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < HISTOGRAM_BINS; i++)
    for(size_t j = 0; j < HISTOGRAM_BINS; j++)
    {
      const size_t index = (HISTOGRAM_BINS - 1 - i) * HISTOGRAM_BINS + j;
      const float value = sqrtf((float)vectorscope[index] / (float)max_hist);
      dt_aligned_pixel_t RGB = { 0.f };
      // RGB gamuts tend to have a max chroma around L = 67
      dt_aligned_pixel_t Luv = { 25.f, _vectorscope_coord_zoom_to_Luv(j, zoom), _vectorscope_coord_zoom_to_Luv(i, zoom), 1.f };
      dt_aligned_pixel_t xyY = { 0.f };
      dt_aligned_pixel_t XYZ = { 0.f };
      dt_Luv_to_xyY(Luv, xyY);
      for(int c = 0; c < 2; c++) xyY[c] = fmaxf(xyY[c], 0.f);
      dt_xyY_to_XYZ(xyY, XYZ);
      for(int c = 0; c < 3; c++) XYZ[c] = fmaxf(XYZ[c], 0.f);
      dt_apply_transposed_color_matrix(XYZ, profile->matrix_out_transposed, RGB);

      // We will normalize RGB to get closer to display peak emission
      for(int c = 0; c < 3; c++) RGB[c] = fmaxf(RGB[c], 0.f);
      const float max_RGB = fmax(RGB[0], fmaxf(RGB[1], RGB[2]));
      for(int c = 0; c < 3; c++) RGB[c] /= max_RGB;

      image[index * 4 + 3] = (uint8_t)roundf(value * 255.f); // alpha
      // Premultiply alpha
      image[index * 4 + 2] = (uint8_t)roundf(powf(RGB[0] * value, 1.f / 2.2f) * 255.f);
      image[index * 4 + 1] = (uint8_t)roundf(powf(RGB[1] * value, 1.f / 2.2f) * 255.f);
      image[index * 4 + 0] = (uint8_t)roundf(powf(RGB[2] * value, 1.f / 2.2f) * 255.f);
    }
}


static void _process_vectorscope(dt_backbuf_t *backbuf, cairo_t *cr, const int width, const int height, const float zoom)
{
  dt_iop_order_iccprofile_info_t *profile = darktable.develop->preview_pipe->output_profile_info;
  if(profile == NULL) return;

  // 1. Process data
  uint32_t *const restrict vectorscope = dt_alloc_align(HISTOGRAM_BINS * HISTOGRAM_BINS * sizeof(uint32_t));
  _bin_pixels_vectorscope(backbuf->buffer, vectorscope, profile, backbuf->width * backbuf->height, zoom);

  const uint32_t max_hist = _find_max_histogram(vectorscope, HISTOGRAM_BINS * HISTOGRAM_BINS);
  uint8_t *const restrict image = dt_alloc_align(4 * HISTOGRAM_BINS * HISTOGRAM_BINS * sizeof(uint8_t));
  _create_vectorscope_image(vectorscope, image, profile, max_hist, zoom);

  // 2. Draw
  if(max_hist > 0)
  {
    const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, HISTOGRAM_BINS);
    cairo_surface_t *background = cairo_image_surface_create_for_data(image, CAIRO_FORMAT_ARGB32, HISTOGRAM_BINS, HISTOGRAM_BINS, stride);
    cairo_translate(cr, (double)(width - height) / 2., 0.);
    cairo_scale(cr, (double)height / HISTOGRAM_BINS, (double)height / HISTOGRAM_BINS);

    const double radius = (float)(HISTOGRAM_BINS - 1) / 2 - DT_PIXEL_APPLY_DPI(1.);
    const double x_center = (float)(HISTOGRAM_BINS - 1) / 2;

    // Background circle - Color will not be exposed to user theme because this is tricky
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_arc(cr, x_center, x_center, radius, 0., 2. * M_PI);
    cairo_fill(cr);

    // Center circle
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_arc(cr, x_center, x_center, 2., 0., 2. * M_PI);
    cairo_fill(cr);

    // Concentric circles
    for(int k = 0; k < 4; k++)
    {
      cairo_arc(cr, x_center, x_center, (double)k * HISTOGRAM_BINS / 8., 0., 2. * M_PI);
      cairo_stroke(cr);
    }

    // RGB space primaries and secondaries
    dt_aligned_pixel_t colors[6] = { { 1.f, 0.f, 0.f, 0.f }, { 1.f, 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f, 0.f },
                                     { 0.f, 1.f, 1.f, 0.f }, { 0.f, 0.f, 1.f, 0.f }, { 1.f, 0.f, 1.f, 0.f } };

    cairo_save(cr);
    cairo_arc(cr, x_center, x_center, radius, 0., 2. * M_PI);
    cairo_clip(cr);

    for(size_t k = 0; k < 6; k++)
    {
      dt_aligned_pixel_t XYZ_D50 = { 0.f };
      dt_aligned_pixel_t xyY = { 0.f };
      dt_aligned_pixel_t Luv = { 0.f };
      dt_ioppr_rgb_matrix_to_xyz(colors[k], XYZ_D50, profile->matrix_in_transposed, profile->lut_in, profile->unbounded_coeffs_in,
                                profile->lutsize, profile->nonlinearlut);
      dt_XYZ_to_xyY(XYZ_D50, xyY);
      dt_xyY_to_Luv(xyY, Luv);

      const double x = _Luv_to_vectorscope_coord_zoom(Luv[1], zoom);
      // Remember v = 0 is at the bottom of the square while Cairo puts y = 0 on top
      const double y = HISTOGRAM_BINS - 1 - _Luv_to_vectorscope_coord_zoom(Luv[2], zoom);

      // First, draw hue angles
      dt_aligned_pixel_t Lch = { 0.f };
      dt_Luv_to_Lch(Luv, Lch);

      const double delta_x = radius * cosf(Lch[2]);
      const double delta_y = radius * sinf(Lch[2]);
      const double destination_x = x_center + delta_x;
      const double destination_y = (HISTOGRAM_BINS - 1) - (x_center + delta_y);
      cairo_move_to(cr, x_center, x_center);
      cairo_line_to(cr, destination_x, destination_y);
      cairo_set_source_rgba(cr, colors[k][0], colors[k][1], colors[k][2], 0.5);
      cairo_stroke(cr);

      // Then draw color squares and ensure center is filled with scope background color
      const double half_square = DT_PIXEL_APPLY_DPI(4);
      cairo_arc(cr, x, y, half_square, 0, 2. * M_PI);
      cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
      cairo_fill_preserve(cr);
      cairo_set_source_rgb(cr, colors[k][0], colors[k][1], colors[k][2]);
      cairo_stroke(cr);
    }
    cairo_restore(cr);

    // Hues ring
    cairo_save(cr);
    cairo_arc(cr, x_center, x_center, radius - DT_PIXEL_APPLY_DPI(1.), 0., 2. * M_PI);
    cairo_set_source_rgb(cr, 0.33, 0.33, 0.33);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
    cairo_stroke(cr);
    cairo_restore(cr);

    for(size_t h = 0; h < 180; h++)
    {
      dt_aligned_pixel_t Lch = { 50.f, 110.f, h / 180.f * 2.f * M_PI_F, 1.f };
      dt_aligned_pixel_t Luv = { 0.f };
      dt_aligned_pixel_t xyY = { 0.f };
      dt_aligned_pixel_t XYZ = { 0.f };
      dt_aligned_pixel_t RGB = { 0.f };
      dt_Lch_to_Luv(Lch, Luv);
      dt_Luv_to_xyY(Luv, xyY);
      dt_xyY_to_XYZ(xyY, XYZ);
      dt_apply_transposed_color_matrix(XYZ, profile->matrix_out_transposed, RGB);
      const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
      for(int c = 0; c < 3; c++) RGB[c] /= max_RGB;
      const double delta_x = (radius - DT_PIXEL_APPLY_DPI(1.)) * cosf(Lch[2]);
      const double delta_y = (radius - DT_PIXEL_APPLY_DPI(1.)) * sinf(Lch[2]);
      const double destination_x = x_center + delta_x;
      const double destination_y = (HISTOGRAM_BINS - 1) - (x_center + delta_y);
      cairo_set_source_rgba(cr, RGB[0], RGB[1], RGB[2], 0.7);
      cairo_arc(cr, destination_x, destination_y, DT_PIXEL_APPLY_DPI(1.), 0, 2. * M_PI);
      cairo_fill(cr);
    }

    // Actual vectorscope
    cairo_arc(cr, x_center, x_center, radius, 0., 2. * M_PI);
    cairo_clip(cr);
    cairo_set_source_surface(cr, background, 0., 0.);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_surface_destroy(background);

    // Draw the skin tones area
    // Values obtained with :
    // get_skin_tones_range();
    const float max_c = 49.34f;
    const float min_c = 9.00f;
    const float max_h = 0.99f;
    const float min_h = 0.26f;

    const float n_w_x = min_c * cosf(max_h);
    const float n_w_y = min_c * sinf(max_h);
    const float n_e_x = max_c * cosf(max_h);
    const float n_e_y = max_c * sinf(max_h);
    const float s_e_x = max_c * cosf(min_h);
    const float s_e_y = max_c * sinf(min_h);
    const float s_w_x = min_c * cosf(min_h);
    const float s_w_y = min_c * sinf(min_h);
    cairo_move_to(cr, _Luv_to_vectorscope_coord_zoom(n_w_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(n_w_y, zoom));
    cairo_line_to(cr, _Luv_to_vectorscope_coord_zoom(n_e_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(n_e_y, zoom));
    cairo_line_to(cr, _Luv_to_vectorscope_coord_zoom(s_e_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(s_e_y, zoom));
    cairo_line_to(cr, _Luv_to_vectorscope_coord_zoom(s_w_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(s_w_y, zoom));
    cairo_line_to(cr, _Luv_to_vectorscope_coord_zoom(n_w_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(n_w_y, zoom));
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_stroke(cr);
  }

  dt_free_align(image);
  dt_free_align(vectorscope);
}


gboolean _needs_recompute(dt_lib_histogram_t *d, const int width, const int height)
{
  // Check if cache is up-to-date
  dt_lib_histogram_scope_type_t view = dt_bauhaus_combobox_get(d->display);
  return !(d->cache.hash == d->backbuf->hash &&
           d->cache.width == width &&
           d->cache.height == height &&
           d->cache.view == view &&
           d->cache.zoom == d->zoom &&
           d->cst == NULL);
}


static gboolean _draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  // Note: the draw callback is called from our own callback (mapped to "preview pipe finished recomputing" signal)
  // but is also called by Gtk when the main window is resized, exposed, etc.
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  if(d->cst == NULL) return 1;
  cairo_set_source_surface(crf, d->cst, 0, 0);
  cairo_paint(crf);
  return 0;
}


void _get_allocation_size(dt_lib_histogram_t *d, int *width, int *height)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(d->scope_draw, &allocation);
  *width = allocation.width;
  *height = allocation.height;
}


gboolean _redraw_surface(dt_lib_histogram_t *d)
{
  if(d->cst == NULL) return 1;

  dt_times_t start;
  dt_get_times(&start);

  int width, height;
  _get_allocation_size(d, &width, &height);

  // Save cache integrity
  d->cache.hash = d->backbuf->hash;
  d->cache.width = width;
  d->cache.height = height;
  d->cache.zoom = d->zoom;
  d->cache.view = dt_bauhaus_combobox_get(d->display);

  cairo_t *cr = cairo_create(d->cst);

  // Paint background
  gtk_render_background(gtk_widget_get_style_context(d->scope_draw), cr, 0, 0, width, height);
  cairo_set_line_width(cr, 1.); // we want exactly 1 px no matter the resolution

  // Paint content
  switch(dt_bauhaus_combobox_get(d->display))
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
    {
      _process_histogram(d->backbuf, cr, width, height);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM_HORIZONTAL:
    {
      _process_waveform(d->backbuf, cr, width, height, FALSE, FALSE);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM_VERTICAL:
    {
      _process_waveform(d->backbuf, cr, width, height, TRUE, FALSE);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_PARADE_HORIZONTAL:
    {
      _process_waveform(d->backbuf, cr, width, height, FALSE, TRUE);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_PARADE_VERTICAL:
    {
      _process_waveform(d->backbuf, cr, width, height, TRUE, TRUE);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
    {
      _process_vectorscope(d->backbuf, cr, width, height, d->zoom);
      break;
    }
    default:
      break;
  }

  cairo_destroy(cr);
  dt_show_times_f(&start, "[histogram]", "redraw");
  return 0;
}

gboolean _trigger_recompute(dt_lib_histogram_t *d)
{
  int width, height;
  _get_allocation_size(d, &width, &height);

  if(_is_backbuf_ready(d) && _needs_recompute(d, width, height))
  {
    if(d->cst && cairo_surface_get_reference_count(d->cst) > 0) cairo_surface_destroy(d->cst);
    // If width and height have changed, we need to recreate the surface.
    // Recreate it anyway.
    d->cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    _redraw_surface(d);
    // Don't send gtk_queue_redraw event from here, catch the return value and do it in the calling function
    return 1;
  }

  return 0;
}


// this is only called in darkroom view when preview pipe finishes
static void _lib_histogram_preview_updated_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->backbuf = _get_backuf(darktable.develop, d->op);
  if(_trigger_recompute(d)) _redraw_scopes(d);
}


void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  dt_lib_histogram_t *d = self->data;
  _reset_cache(d);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                  G_CALLBACK(_lib_histogram_preview_updated_callback), self);
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  dt_lib_histogram_t *d = self->data;
  _reset_cache(d);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_histogram_preview_updated_callback), self);
}


void _stage_callback(GtkWidget *widget, dt_lib_histogram_t *d)
{
  const int value = dt_bauhaus_combobox_get(widget);
  _backbuf_int_to_op(value, d);
  dt_conf_set_string("plugin/darkroom/histogram/op", d->op);

  // Disable vectorscope for RAW stage
  dt_bauhaus_combobox_entry_set_sensitive(d->display, DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE, strcmp(d->op, "demosaic"));

  d->backbuf = _get_backuf(darktable.develop, d->op);
  if(_trigger_recompute(d)) _redraw_scopes(d);
}


void _display_callback(GtkWidget *widget, dt_lib_histogram_t *d)
{
  dt_conf_set_int("plugin/darkroom/histogram/display", dt_bauhaus_combobox_get(d->display));
  if(_trigger_recompute(d)) _redraw_scopes(d);
}


static void _resize_callback(GtkWidget *widget, GdkRectangle *allocation, dt_lib_histogram_t *d)
{
  _reset_cache(d);
  _trigger_recompute(d);
  // Don't start a redraw from here, Gtk does it automatically on resize event
}

static gboolean _area_scrolled_callback(GtkWidget *widget, GdkEventScroll *event, dt_lib_histogram_t *d)
{
  if(dt_bauhaus_combobox_get(d->display) != DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE) return FALSE;

  int delta_y;
  dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y);
  const float new_value = 4.f * delta_y + d->zoom;

  if(new_value < 512.f && new_value > 32.f)
  {
    d->zoom = new_value;
    dt_conf_set_float("plugin/darkroom/histogram/zoom", new_value);
    if(_is_backbuf_ready(d))
    {
      _redraw_surface(d);
      _redraw_scopes(d);
    }
  }
  return TRUE;
}

void _set_params(dt_lib_histogram_t *d)
{
  d->op = dt_conf_get_string_const("plugin/darkroom/histogram/op");
  d->backbuf = _get_backuf(darktable.develop, d->op);
  d->zoom = fminf(fmaxf(dt_conf_get_float("plugin/darkroom/histogram/zoom"), 32.f), 252.f);

  // Disable RAW stage for non-RAW images
  dt_bauhaus_combobox_entry_set_sensitive(d->stage, 0, dt_image_is_raw(&darktable.develop->image_storage));

  // Disable vectorscope if RAW stage is selected
  dt_bauhaus_combobox_entry_set_sensitive(d->display, DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE, strcmp(d->op, "demosaic"));

  dt_bauhaus_combobox_set(d->display, dt_conf_get_int("plugin/darkroom/histogram/display"));
  dt_bauhaus_combobox_set(d->stage, _backbuf_op_to_int(d));
}


void gui_reset(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;
  _reset_cache(d);
  _set_params(d);
  if(d->cst && cairo_surface_get_reference_count(d->cst) > 0) cairo_surface_destroy(d->cst);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)dt_calloc_align(sizeof(dt_lib_histogram_t));
  self->data = (void *)d;
  d->cst = NULL;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  d->scope_draw = dtgtk_drawing_area_new_with_aspect_ratio(1.);
  gtk_widget_add_events(GTK_WIDGET(d->scope_draw), darktable.gui->scroll_mask);
  gtk_widget_set_size_request(d->scope_draw, -1, DT_PIXEL_APPLY_DPI(250));
  g_signal_connect(G_OBJECT(d->scope_draw), "draw", G_CALLBACK(_draw_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "scroll-event", G_CALLBACK(_area_scrolled_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "size-allocate", G_CALLBACK(_resize_callback), d);
  gtk_box_pack_start(GTK_BOX(self->widget), d->scope_draw, TRUE, TRUE, 0);

  d->stage = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->stage, NULL, _("Show data from"));
  dt_bauhaus_combobox_add(d->stage, _("Raw image"));
  dt_bauhaus_combobox_add(d->stage, _("Output color profile"));
  dt_bauhaus_combobox_add(d->stage, _("Final display"));
  g_signal_connect(G_OBJECT(d->stage), "value-changed", G_CALLBACK(_stage_callback), d);
  gtk_box_pack_start(GTK_BOX(self->widget), d->stage, FALSE, FALSE, 0);

  d->display = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->display, NULL, _("Display"));
  dt_bauhaus_combobox_add(d->display, _("Histogram"));
  dt_bauhaus_combobox_add(d->display, _("Waveform (horizontal)"));
  dt_bauhaus_combobox_add(d->display, _("Waveform (vertical)"));
  dt_bauhaus_combobox_add(d->display, _("Parade (horizontal)"));
  dt_bauhaus_combobox_add(d->display, _("Parade (vertical)"));
  dt_bauhaus_combobox_add(d->display, _("Vectorscope"));
  g_signal_connect(G_OBJECT(d->display), "value-changed", G_CALLBACK(_display_callback), d);
  gtk_box_pack_start(GTK_BOX(self->widget), d->display, FALSE, FALSE, 0);

  _reset_cache(d);
  _set_params(d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;
  if(d->cst && cairo_surface_get_reference_count(d->cst) > 0) cairo_surface_destroy(d->cst);
  dt_free_align(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
