/*
    This file is part of the Ansel project.
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

#ifdef HAVE_CONFIG_H
#include "common/darktable.h"
#include "config.h"
#endif

#include "iop/drawlayer/widgets.h"

#include "iop/drawlayer/paint.h"
#include "gui/gtk.h"

#include <float.h>
#include <math.h>
#include <string.h>

/** @file
 *  @brief Drawlayer widget helpers (picker, swatch, color history).
 */

#define DT_DRAWLAYER_PICKER_U_MAX 0.70710678f
#define DT_DRAWLAYER_PICKER_V_MAX 0.81649658f
#define DT_DRAWLAYER_PICKER_C_MAX 0.81649658f

/** @brief Active drag target in picker UI. */
typedef enum dt_drawlayer_color_drag_mode_t
{
  DT_DRAWLAYER_COLOR_DRAG_NONE = 0,
  DT_DRAWLAYER_COLOR_DRAG_DISC = 1,
  DT_DRAWLAYER_COLOR_DRAG_PLANE = 2,
} dt_drawlayer_color_drag_mode_t;

/** @brief Runtime state for drawlayer custom color widgets. */
struct dt_drawlayer_widgets_t
{
  float picker_m;        /**< Achromatic component in opponent space. */
  float picker_u;        /**< Opponent chroma axis U. */
  float picker_v;        /**< Opponent chroma axis V. */
  float picker_hue;      /**< Polar hue derived from U/V. */
  float picker_chroma;   /**< Polar chroma derived from U/V. */
  int picker_drag_mode;  /**< Active drag mode (disc/plane/none). */

  float color_history[DT_DRAWLAYER_COLOR_HISTORY_COUNT][3]; /**< Display-RGB history stack. */
  gboolean color_history_valid[DT_DRAWLAYER_COLOR_HISTORY_COUNT]; /**< Validity flags per history slot. */

  cairo_surface_t *color_surface;  /**< Cached picker surface. */
  int color_surface_width;         /**< Cached surface width in pixels. */
  int color_surface_height;        /**< Cached surface height in pixels. */
  double color_surface_ppd;        /**< Pixels-per-dip used for surface cache. */
  gboolean color_surface_dirty;    /**< Surface requires rebuild on next draw. */

  cairo_surface_t *profile_surface; /**< Cached brush-profile preview row. */
  int profile_surface_width;        /**< Cached profile surface width in pixels. */
  int profile_surface_height;       /**< Cached profile surface height in pixels. */
  double profile_surface_ppd;       /**< Pixels-per-dip used for profile surface cache. */
  gboolean profile_surface_dirty;   /**< Surface requires rebuild on next draw. */
  float profile_opacity;            /**< Preview opacity in [0,1]. */
  float profile_hardness;           /**< Preview hardness in [0,1]. */
  float profile_sprinkles;          /**< Preview sprinkles in [0,1]. */
  float profile_sprinkle_size;      /**< Preview sprinkle size in px. */
  float profile_sprinkle_coarseness;/**< Preview sprinkle octave mix in [0,1]. */
  int profile_selected_shape;       /**< Currently selected brush profile enum. */
};

/** @brief Clamp scalar to [0,1]. */
static inline float _clamp01(const float value)
{
  return fminf(fmaxf(value, 0.0f), 1.0f);
}

/** @brief Compare display-RGB triplets with tiny epsilon tolerance. */
static gboolean _display_rgb_equal(const float a[3], const float b[3])
{
  return fabsf(a[0] - b[0]) <= 1e-6f && fabsf(a[1] - b[1]) <= 1e-6f && fabsf(a[2] - b[2]) <= 1e-6f;
}

/** @brief Update hue/chroma from current U/V components. */
static void _picker_update_polar_from_uv(dt_drawlayer_widgets_t *widgets)
{
  if(IS_NULL_PTR(widgets)) return;
  widgets->picker_hue = atan2f(widgets->picker_v, widgets->picker_u);
  widgets->picker_chroma = hypotf(widgets->picker_u, widgets->picker_v);
}

/** @brief Update U/V from current hue/chroma components. */
static void _picker_update_uv_from_polar(dt_drawlayer_widgets_t *widgets)
{
  if(IS_NULL_PTR(widgets)) return;
  widgets->picker_u = widgets->picker_chroma * cosf(widgets->picker_hue);
  widgets->picker_v = widgets->picker_chroma * sinf(widgets->picker_hue);
}

/** @brief Project opponent-space color to display RGB; fail when out of gamut. */
static int _picker_project_opponent_to_display_rgb(const float m, const float u, const float v, float display_rgb[3])
{
  const float r = m + u * 0.70710678f + v * 0.40824829f;
  const float g = m - u * 0.70710678f + v * 0.40824829f;
  const float b = m - v * 0.81649658f;

  if(!dt_isfinite(r) || !dt_isfinite(g) || !dt_isfinite(b)) return 1;
  if(r < 0.0f || r > 1.0f || g < 0.0f || g > 1.0f || b < 0.0f || b > 1.0f) return 1;

  display_rgb[0] = r;
  display_rgb[1] = g;
  display_rgb[2] = b;
  return 0;
}

/** @brief Compute max reachable chroma at given lightness/hue within display gamut. */
static float _picker_max_chroma_for_m_hue(const float m, const float hue)
{
  const float ku = cosf(hue);
  const float kv = sinf(hue);
  const float k[3] = {
    ku * 0.70710678f + kv * 0.40824829f,
    -ku * 0.70710678f + kv * 0.40824829f,
    -kv * 0.81649658f
  };

  if(m <= 0.0f || m >= 1.0f) return 0.0f;

  float limit = FLT_MAX;
  for(int c = 0; c < 3; c++)
  {
    if(k[c] > 1e-6f)
      limit = fminf(limit, (1.0f - m) / k[c]);
    else if(k[c] < -1e-6f)
      limit = fminf(limit, m / -k[c]);
  }

  if(!dt_isfinite(limit) || limit < 0.0f) return 0.0f;
  return limit;
}

/** @brief Clamp picker state so projected display RGB always remains valid. */
static void _picker_clamp_state_to_gamut(dt_drawlayer_widgets_t *widgets)
{
  if(IS_NULL_PTR(widgets)) return;

  widgets->picker_m = CLAMP(widgets->picker_m, 0.0f, 1.0f);
  _picker_update_polar_from_uv(widgets);
  const float max_chroma = _picker_max_chroma_for_m_hue(widgets->picker_m, widgets->picker_hue);
  if(widgets->picker_chroma > max_chroma)
  {
    widgets->picker_chroma = max_chroma;
    _picker_update_uv_from_polar(widgets);
  }
}

/** @brief Initialize picker opponent coordinates from display RGB color. */
static void _sync_picker_from_display_rgb(dt_drawlayer_widgets_t *widgets, const float display_rgb[3])
{
  if(IS_NULL_PTR(widgets) || !display_rgb) return;

  widgets->picker_m = (display_rgb[0] + display_rgb[1] + display_rgb[2]) / 3.0f;
  widgets->picker_u = (display_rgb[0] - display_rgb[1]) * 0.70710678f;
  widgets->picker_v = (display_rgb[0] + display_rgb[1] - 2.0f * display_rgb[2]) * 0.40824829f;
  _picker_clamp_state_to_gamut(widgets);
}

/** @brief Destroy cached picker surface and reset cache metadata. */
static void _clear_color_picker_surface(dt_drawlayer_widgets_t *widgets)
{
  if(IS_NULL_PTR(widgets)) return;
  if(widgets->color_surface)
  {
    cairo_surface_destroy(widgets->color_surface);
    widgets->color_surface = NULL;
  }
  widgets->color_surface_width = 0;
  widgets->color_surface_height = 0;
  widgets->color_surface_ppd = 0.0;
  widgets->color_surface_dirty = TRUE;
}

/** @brief Destroy cached brush-profile preview surface and reset cache metadata. */
static void _clear_profile_surface(dt_drawlayer_widgets_t *widgets)
{
  if(IS_NULL_PTR(widgets)) return;
  if(widgets->profile_surface)
  {
    cairo_surface_destroy(widgets->profile_surface);
    widgets->profile_surface = NULL;
  }
  widgets->profile_surface_width = 0;
  widgets->profile_surface_height = 0;
  widgets->profile_surface_ppd = 0.0;
  widgets->profile_surface_dirty = TRUE;
}

/** @brief Compute profile-row geometry inside widget. */
static void _brush_profile_geometry(const GtkWidget *widget, float *x, float *y, float *width, float *height,
                                    float *gap, int *cell_count)
{
  const float margin = 0.0f;
  const float local_gap = DT_PIXEL_APPLY_DPI(6.0f);
  const int count = 4;
  const float widget_w = gtk_widget_get_allocated_width((GtkWidget *)widget);
  const float widget_h = gtk_widget_get_allocated_height((GtkWidget *)widget);
  if(!IS_NULL_PTR(x)) *x = margin;
  if(!IS_NULL_PTR(y)) *y = margin;
  if(!IS_NULL_PTR(width)) *width = fmaxf(24.0f, widget_w - 2.0f * margin);
  if(!IS_NULL_PTR(height)) *height = fmaxf(24.0f, widget_h - 2.0f * margin);
  if(!IS_NULL_PTR(gap)) *gap = local_gap;
  if(!IS_NULL_PTR(cell_count)) *cell_count = count;
}

/** @brief Compute one profile-cell rectangle. */
static gboolean _brush_profile_cell_rect(const GtkWidget *widget, const int index,
                                         float *x, float *y, float *width, float *height)
{
  float row_x = 0.0f, row_y = 0.0f, row_w = 0.0f, row_h = 0.0f, gap = 0.0f;
  int count = 0;
  _brush_profile_geometry(widget, &row_x, &row_y, &row_w, &row_h, &gap, &count);
  if(index < 0 || index >= count) return FALSE;

  const float cell_w = fmaxf(8.0f, (row_w - gap * (count - 1)) / (float)count);
  if(!IS_NULL_PTR(x)) *x = row_x + index * (cell_w + gap);
  if(!IS_NULL_PTR(y)) *y = row_y;
  if(!IS_NULL_PTR(width)) *width = cell_w;
  if(!IS_NULL_PTR(height)) *height = row_h;
  return TRUE;
}

/** @brief Convert linear float channel to display-encoded 8-bit channel. */
static inline uint8_t _linear_channel_to_u8(const float x)
{
  const float v = _clamp01(x);
  const float srgb = (v <= 0.0031308f) ? (12.92f * v) : (1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
  return (uint8_t)CLAMP((int)lrintf(255.0f * _clamp01(srgb)), 0, 255);
}

/** @brief Render one brush preview cell into ARGB8 memory. */
static void _render_brush_profile_cell(unsigned char *dst, const int stride, const int width, const int height,
                                       float *rgba_scratch,
                                       const dt_drawlayer_widgets_t *widgets, const int shape)
{
  if(IS_NULL_PTR(dst) || IS_NULL_PTR(rgba_scratch) || width <= 0 || height <= 0 || IS_NULL_PTR(widgets)) return;

  memset(dst, 0, (size_t)stride * height);
  for(int py = 0; py < height; py++)
  {
    unsigned char *row = dst + (size_t)py * stride;
    for(int px = 0; px < width; px++)
    {
      row[4 * px + 1] = 0xff;
      row[4 * px + 2] = 0xff;
      row[4 * px + 3] = 0xff;
      row[4 * px + 0] = 0xff;
    }
  }

  const float radius = 0.36f * fminf((float)width, (float)height);
  dt_drawlayer_brush_dab_t dab = {
    .x = 0.0f,
    .y = 0.0f,
    .wx = 0.0f,
    .wy = 0.0f,
    .radius = fmaxf(radius, 1.0f),
    .dir_x = 0.0f,
    .dir_y = 1.0f,
    .opacity = _clamp01(widgets->profile_opacity),
    .flow = 0.0f,
    .sprinkles = _clamp01(widgets->profile_sprinkles),
    .sprinkle_size = fmaxf(1.0f, widgets->profile_sprinkle_size),
    .sprinkle_coarseness = _clamp01(widgets->profile_sprinkle_coarseness),
    .hardness = _clamp01(widgets->profile_hardness),
    .color = { 0.0f, 0.0f, 0.0f, 1.0f },
    .display_color = { 0.0f, 0.0f, 0.0f },
    .shape = shape,
    .mode = DT_DRAWLAYER_BRUSH_MODE_PAINT,
    .stroke_batch = 0u,
    .stroke_pos = 0u,
  };

  const float bg[3] = { 1.0f, 1.0f, 1.0f };
  dt_drawlayer_brush_rasterize_dab_rgbaf(&dab, rgba_scratch, width, height,
                                         0.5f * (float)width, 0.5f * (float)height, 1.0f, bg);

  for(int y = 0; y < height; y++)
  {
    unsigned char *row = dst + (size_t)y * stride;
    for(int x = 0; x < width; x++)
    {
      const float *pixel = rgba_scratch + 4 * ((size_t)y * width + x);
      row[4 * x + 0] = _linear_channel_to_u8(pixel[2]);
      row[4 * x + 1] = _linear_channel_to_u8(pixel[1]);
      row[4 * x + 2] = _linear_channel_to_u8(pixel[0]);
      row[4 * x + 3] = 255;
    }
  }
}

/** @brief Compute picker-disc and value/chroma-plane geometry inside widget. */
static void _color_picker_geometry(const GtkWidget *widget, float *uv_x, float *uv_y, float *uv_size,
                                   float *plane_x, float *plane_y, float *plane_w, float *plane_h)
{
  const float width = gtk_widget_get_allocated_width((GtkWidget *)widget);
  const float height = gtk_widget_get_allocated_height((GtkWidget *)widget);
  const float margin = DT_PIXEL_APPLY_DPI(6.0f);
  const float gap = DT_PIXEL_APPLY_DPI(16.0f);
  const float usable_w = fmaxf(40.0f, width - 2.0f * margin - gap);
  const float usable_h = fmaxf(40.0f, height - 2.0f * margin);
  const float size = fmaxf(40.0f, fminf(usable_h, 0.5f * usable_w));
  if(!IS_NULL_PTR(uv_x)) *uv_x = margin;
  if(!IS_NULL_PTR(uv_y)) *uv_y = margin + 0.5f * (usable_h - size);
  if(!IS_NULL_PTR(uv_size)) *uv_size = size;
  if(!IS_NULL_PTR(plane_x)) *plane_x = margin + size + gap;
  if(!IS_NULL_PTR(plane_y)) *plane_y = margin + 0.5f * (usable_h - size);
  if(!IS_NULL_PTR(plane_w)) *plane_w = size;
  if(!IS_NULL_PTR(plane_h)) *plane_h = size;
}

/** @brief Hit-margin in device-independent pixels for drag continuity. */
static float _color_picker_hit_margin(void)
{
  const float ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0f;
  return DT_PIXEL_APPLY_DPI(12.0f) * ppd;
}

/** @brief Rectangle hit-test with configurable margin expansion. */
static gboolean _rect_contains_with_margin(const float x, const float y, const float rx, const float ry,
                                           const float rw, const float rh, const float margin)
{
  return x >= rx - margin && x <= rx + rw + margin && y >= ry - margin && y <= ry + rh + margin;
}

/** @brief Allocate and initialize widget runtime state. */
dt_drawlayer_widgets_t *dt_drawlayer_widgets_init(void)
{
  dt_drawlayer_widgets_t *widgets = g_malloc0(sizeof(*widgets));
  widgets->picker_drag_mode = DT_DRAWLAYER_COLOR_DRAG_NONE;
  widgets->color_surface_dirty = TRUE;
  widgets->profile_surface_dirty = TRUE;
  widgets->profile_opacity = 1.0f;
  widgets->profile_hardness = 0.5f;
  widgets->profile_sprinkle_size = 3.0f;
  widgets->profile_sprinkle_coarseness = 0.5f;
  widgets->profile_selected_shape = DT_DRAWLAYER_BRUSH_SHAPE_LINEAR;
  return widgets;
}

/** @brief Free widget runtime state and owned cairo resources. */
void dt_drawlayer_widgets_cleanup(dt_drawlayer_widgets_t **widgets)
{
  if(IS_NULL_PTR(widgets) || !*widgets) return;
  _clear_color_picker_surface(*widgets);
  _clear_profile_surface(*widgets);
  dt_free(*widgets);
}

/** @brief Set current color and synchronize picker internals. */
void dt_drawlayer_widgets_set_display_color(dt_drawlayer_widgets_t *widgets, const float display_rgb[3])
{
  if(IS_NULL_PTR(widgets) || !display_rgb) return;
  _sync_picker_from_display_rgb(widgets, display_rgb);
  widgets->color_surface_dirty = TRUE;
}

/** @brief Get current color projected to display RGB. */
gboolean dt_drawlayer_widgets_get_display_color(const dt_drawlayer_widgets_t *widgets, float display_rgb[3])
{
  if(!widgets || !display_rgb) return FALSE;
  return _picker_project_opponent_to_display_rgb(widgets->picker_m, widgets->picker_u, widgets->picker_v, display_rgb) == 0;
}

/** @brief Mark picker surface dirty for regeneration on next draw. */
void dt_drawlayer_widgets_mark_picker_dirty(dt_drawlayer_widgets_t *widgets)
{
  if(IS_NULL_PTR(widgets)) return;
  widgets->color_surface_dirty = TRUE;
}

/** @brief Replace full color-history content and validity flags. */
void dt_drawlayer_widgets_set_color_history(dt_drawlayer_widgets_t *widgets,
                                            const float history[DT_DRAWLAYER_COLOR_HISTORY_COUNT][3],
                                            const gboolean valid[DT_DRAWLAYER_COLOR_HISTORY_COUNT])
{
  if(IS_NULL_PTR(widgets) || !history || !valid) return;
  for(int i = 0; i < DT_DRAWLAYER_COLOR_HISTORY_COUNT; i++)
  {
    widgets->color_history_valid[i] = valid[i];
    widgets->color_history[i][0] = _clamp01(history[i][0]);
    widgets->color_history[i][1] = _clamp01(history[i][1]);
    widgets->color_history[i][2] = _clamp01(history[i][2]);
  }
}

/** @brief Copy full color-history content and validity flags out. */
void dt_drawlayer_widgets_get_color_history(const dt_drawlayer_widgets_t *widgets,
                                            float history[DT_DRAWLAYER_COLOR_HISTORY_COUNT][3],
                                            gboolean valid[DT_DRAWLAYER_COLOR_HISTORY_COUNT])
{
  if(IS_NULL_PTR(widgets) || !history || !valid) return;
  for(int i = 0; i < DT_DRAWLAYER_COLOR_HISTORY_COUNT; i++)
  {
    valid[i] = widgets->color_history_valid[i];
    history[i][0] = widgets->color_history[i][0];
    history[i][1] = widgets->color_history[i][1];
    history[i][2] = widgets->color_history[i][2];
  }
}

/** @brief Push one color to history head if different from current head. */
gboolean dt_drawlayer_widgets_push_color_history(dt_drawlayer_widgets_t *widgets, const float display_rgb[3])
{
  if(!widgets || !display_rgb) return FALSE;

  if(widgets->color_history_valid[0] && _display_rgb_equal(widgets->color_history[0], display_rgb)) return FALSE;

  for(int i = DT_DRAWLAYER_COLOR_HISTORY_COUNT - 1; i > 0; i--)
  {
    widgets->color_history_valid[i] = widgets->color_history_valid[i - 1];
    widgets->color_history[i][0] = widgets->color_history[i - 1][0];
    widgets->color_history[i][1] = widgets->color_history[i - 1][1];
    widgets->color_history[i][2] = widgets->color_history[i - 1][2];
  }

  widgets->color_history_valid[0] = TRUE;
  widgets->color_history[0][0] = _clamp01(display_rgb[0]);
  widgets->color_history[0][1] = _clamp01(display_rgb[1]);
  widgets->color_history[0][2] = _clamp01(display_rgb[2]);
  return TRUE;
}

/** @brief Update picker state from mouse position and return selected color. */
gboolean dt_drawlayer_widgets_update_from_picker_position(dt_drawlayer_widgets_t *widgets, GtkWidget *widget,
                                                          float x, float y, float display_rgb[3])
{
  if(!widgets || !widget || !display_rgb) return FALSE;

  float uv_x = 0.0f, uv_y = 0.0f, uv_size = 0.0f;
  float plane_x = 0.0f, plane_y = 0.0f, plane_w = 0.0f, plane_h = 0.0f;
  _color_picker_geometry(widget, &uv_x, &uv_y, &uv_size, &plane_x, &plane_y, &plane_w, &plane_h);
  const float hit_margin
      = (widgets->picker_drag_mode == DT_DRAWLAYER_COLOR_DRAG_NONE) ? 0.0f : _color_picker_hit_margin();
  const gboolean in_uv = _rect_contains_with_margin(x, y, uv_x, uv_y, uv_size, uv_size,
                                                     (widgets->picker_drag_mode == DT_DRAWLAYER_COLOR_DRAG_DISC)
                                                         ? hit_margin
                                                         : 0.0f);
  const gboolean in_plane = _rect_contains_with_margin(x, y, plane_x, plane_y, plane_w, plane_h,
                                                        (widgets->picker_drag_mode == DT_DRAWLAYER_COLOR_DRAG_PLANE)
                                                            ? hit_margin
                                                            : 0.0f);

  if(in_uv)
  {
    const float tx = 2.0f * _clamp01((x - uv_x) / fmaxf(uv_size, 1.0f)) - 1.0f;
    const float ty = 1.0f - 2.0f * _clamp01((y - uv_y) / fmaxf(uv_size, 1.0f));
    widgets->picker_u = tx * DT_DRAWLAYER_PICKER_U_MAX;
    widgets->picker_v = ty * DT_DRAWLAYER_PICKER_V_MAX;
    _picker_clamp_state_to_gamut(widgets);
    widgets->picker_drag_mode = DT_DRAWLAYER_COLOR_DRAG_DISC;
  }
  else if(in_plane)
  {
    const float tx = _clamp01((x - plane_x) / fmaxf(plane_w, 1.0f));
    const float ty = _clamp01((y - plane_y) / fmaxf(plane_h, 1.0f));
    const float new_m = 1.0f - ty;
    const float new_chroma = tx * DT_DRAWLAYER_PICKER_C_MAX;
    const float new_u = new_chroma * cosf(widgets->picker_hue);
    const float new_v = new_chroma * sinf(widgets->picker_hue);
    if(_picker_project_opponent_to_display_rgb(new_m, new_u, new_v, display_rgb)) return FALSE;

    widgets->picker_m = new_m;
    widgets->picker_chroma = new_chroma;
    widgets->picker_u = new_u;
    widgets->picker_v = new_v;
    widgets->picker_drag_mode = DT_DRAWLAYER_COLOR_DRAG_PLANE;
  }
  else
  {
    return FALSE;
  }

  _picker_clamp_state_to_gamut(widgets);
  if(!_picker_project_opponent_to_display_rgb(widgets->picker_m, widgets->picker_u, widgets->picker_v, display_rgb))
  {
    widgets->color_surface_dirty = TRUE;
    return TRUE;
  }
  return FALSE;
}

/** @brief End picker drag mode and optionally output current color. */
gboolean dt_drawlayer_widgets_finish_picker_drag(dt_drawlayer_widgets_t *widgets, float display_rgb[3])
{
  if(IS_NULL_PTR(widgets)) return FALSE;
  widgets->picker_drag_mode = DT_DRAWLAYER_COLOR_DRAG_NONE;
  if(IS_NULL_PTR(display_rgb)) return FALSE;
  return !_picker_project_opponent_to_display_rgb(widgets->picker_m, widgets->picker_u, widgets->picker_v, display_rgb);
}

/** @brief Tell whether picker drag interaction is currently active. */
gboolean dt_drawlayer_widgets_is_picker_dragging(const dt_drawlayer_widgets_t *widgets)
{
  return widgets && widgets->picker_drag_mode != DT_DRAWLAYER_COLOR_DRAG_NONE;
}

/** @brief Pick one history swatch color from widget coordinates. */
gboolean dt_drawlayer_widgets_pick_history_color(const dt_drawlayer_widgets_t *widgets, GtkWidget *widget,
                                                 float x, float y, float display_rgb[3])
{
  if(!widgets || !widget || !display_rgb) return FALSE;

  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  if(width <= 0 || height <= 0) return FALSE;

  const int col = CLAMP((int)floorf(x / ((float)width / DT_DRAWLAYER_COLOR_HISTORY_COLS)), 0,
                        DT_DRAWLAYER_COLOR_HISTORY_COLS - 1);
  const int row = CLAMP((int)floorf(y / ((float)height / DT_DRAWLAYER_COLOR_HISTORY_ROWS)), 0,
                        DT_DRAWLAYER_COLOR_HISTORY_ROWS - 1);
  const int index = row * DT_DRAWLAYER_COLOR_HISTORY_COLS + col;
  if(index < 0 || index >= DT_DRAWLAYER_COLOR_HISTORY_COUNT || !widgets->color_history_valid[index]) return FALSE;

  display_rgb[0] = widgets->color_history[index][0];
  display_rgb[1] = widgets->color_history[index][1];
  display_rgb[2] = widgets->color_history[index][2];
  return TRUE;
}

/** @brief Draw color picker map, controls and selection markers. */
gboolean dt_drawlayer_widgets_draw_picker(dt_drawlayer_widgets_t *widgets, GtkWidget *widget, cairo_t *cr,
                                          double pixels_per_dip)
{
  if(!widgets || !widget || IS_NULL_PTR(cr)) return FALSE;

  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  if(width <= 0 || height <= 0) return FALSE;

  const double ppd = (pixels_per_dip > 0.0) ? pixels_per_dip : 1.0;
  const int width_px = MAX(1, (int)ceil(width * ppd));
  const int height_px = MAX(1, (int)ceil(height * ppd));
  const gboolean size_changed = !widgets->color_surface || widgets->color_surface_width != width_px
                                || widgets->color_surface_height != height_px
                                || fabs(widgets->color_surface_ppd - ppd) > 1e-9;

  if(size_changed)
  {
    _clear_color_picker_surface(widgets);
    widgets->color_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_px, height_px);
    if(cairo_surface_status(widgets->color_surface) != CAIRO_STATUS_SUCCESS)
    {
      _clear_color_picker_surface(widgets);
      return FALSE;
    }
    cairo_surface_set_device_scale(widgets->color_surface, ppd, ppd);
    widgets->color_surface_width = width_px;
    widgets->color_surface_height = height_px;
    widgets->color_surface_ppd = ppd;
    widgets->color_surface_dirty = TRUE;
  }

  float uv_x = 0.0f, uv_y = 0.0f, uv_size = 0.0f;
  float plane_x = 0.0f, plane_y = 0.0f, plane_w = 0.0f, plane_h = 0.0f;
  _color_picker_geometry(widget, &uv_x, &uv_y, &uv_size, &plane_x, &plane_y, &plane_w, &plane_h);

  if(widgets->color_surface_dirty)
  {
    unsigned char *data = cairo_image_surface_get_data(widgets->color_surface);
    const int stride = cairo_image_surface_get_stride(widgets->color_surface);
    memset(data, 0, (size_t)stride * height_px);

    for(int py = 0; py < height_px; py++)
    {
      for(int px = 0; px < width_px; px++)
      {
        float rgb[3] = { 0.12f, 0.12f, 0.12f };
        gboolean paint = TRUE;
        const float fx = ((float)px + 0.5f) / ppd;
        const float fy = ((float)py + 0.5f) / ppd;

        if(fx >= uv_x && fx <= uv_x + uv_size && fy >= uv_y && fy <= uv_y + uv_size)
        {
          const float tx = 2.0f * _clamp01((fx - uv_x) / fmaxf(uv_size, 1.0f)) - 1.0f;
          const float ty = 1.0f - 2.0f * _clamp01((fy - uv_y) / fmaxf(uv_size, 1.0f));
          const float u = tx * DT_DRAWLAYER_PICKER_U_MAX;
          const float v = ty * DT_DRAWLAYER_PICKER_V_MAX;
          if(_picker_project_opponent_to_display_rgb(widgets->picker_m, u, v, rgb)) paint = FALSE;
        }
        else if(fx >= plane_x && fx <= plane_x + plane_w && fy >= plane_y && fy <= plane_y + plane_h)
        {
          const float tx = _clamp01((fx - plane_x) / fmaxf(plane_w, 1.0f));
          const float ty = _clamp01((fy - plane_y) / fmaxf(plane_h, 1.0f));
          const float m = 1.0f - ty;
          const float chroma = tx * DT_DRAWLAYER_PICKER_C_MAX;
          const float u = chroma * cosf(widgets->picker_hue);
          const float v = chroma * sinf(widgets->picker_hue);
          if(_picker_project_opponent_to_display_rgb(m, u, v, rgb)) paint = FALSE;
        }
        else
        {
          paint = FALSE;
        }

        unsigned char *pixel = data + (size_t)py * stride + 4 * px;
        if(paint)
        {
          pixel[0] = (unsigned char)CLAMP(roundf(255.0f * _clamp01(rgb[2])), 0, 255);
          pixel[1] = (unsigned char)CLAMP(roundf(255.0f * _clamp01(rgb[1])), 0, 255);
          pixel[2] = (unsigned char)CLAMP(roundf(255.0f * _clamp01(rgb[0])), 0, 255);
          pixel[3] = 255;
        }
        else
        {
          pixel[0] = pixel[1] = pixel[2] = 0;
          pixel[3] = 0;
        }
      }
    }
    cairo_surface_mark_dirty(widgets->color_surface);
    widgets->color_surface_dirty = FALSE;
  }

  cairo_set_source_surface(cr, widgets->color_surface, 0.0, 0.0);
  cairo_paint(cr);

  const float uv_mark_x
      = uv_x + 0.5f * ((widgets->picker_u / fmaxf(DT_DRAWLAYER_PICKER_U_MAX, 1e-6f)) + 1.0f) * uv_size;
  const float uv_mark_y
      = uv_y + 0.5f * (1.0f - (widgets->picker_v / fmaxf(DT_DRAWLAYER_PICKER_V_MAX, 1e-6f))) * uv_size;
  const float plane_mark_x = plane_x + _clamp01(widgets->picker_chroma / DT_DRAWLAYER_PICKER_C_MAX) * plane_w;
  const float plane_mark_y = plane_y + (1.0f - _clamp01(widgets->picker_m)) * plane_h;
  const float mark_r = 5.0f;

  cairo_set_line_width(cr, 2.0);
  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_arc(cr, uv_mark_x, uv_mark_y, mark_r + 1.5f, 0.0, 2.0 * G_PI);
  cairo_stroke(cr);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_arc(cr, uv_mark_x, uv_mark_y, mark_r, 0.0, 2.0 * G_PI);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_arc(cr, plane_mark_x, plane_mark_y, mark_r + 1.5f, 0.0, 2.0 * G_PI);
  cairo_stroke(cr);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_arc(cr, plane_mark_x, plane_mark_y, mark_r, 0.0, 2.0 * G_PI);
  cairo_stroke(cr);

  return FALSE;
}

/** @brief Draw compact color-history swatch grid. */
gboolean dt_drawlayer_widgets_draw_swatch(const dt_drawlayer_widgets_t *widgets, GtkWidget *widget, cairo_t *cr)
{
  if(!widgets || !widget || IS_NULL_PTR(cr)) return FALSE;

  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  if(width <= 0 || height <= 0) return FALSE;

  const float cell_w = (float)width / DT_DRAWLAYER_COLOR_HISTORY_COLS;
  const float cell_h = (float)height / DT_DRAWLAYER_COLOR_HISTORY_ROWS;
  const float gap = DT_PIXEL_APPLY_DPI(3.0f);

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.12);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_fill(cr);

  for(int i = 0; i < DT_DRAWLAYER_COLOR_HISTORY_COUNT; i++)
  {
    const int row = i / DT_DRAWLAYER_COLOR_HISTORY_COLS;
    const int col = i % DT_DRAWLAYER_COLOR_HISTORY_COLS;
    const float x = col * cell_w;
    const float y = row * cell_h;

    if(widgets->color_history_valid[i])
      cairo_set_source_rgb(cr, widgets->color_history[i][0], widgets->color_history[i][1], widgets->color_history[i][2]);
    else
      cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);

    cairo_rectangle(cr, x + gap * 0.5f, y + gap * 0.5f, MAX(1.0f, cell_w - gap), MAX(1.0f, cell_h - gap));
    cairo_fill(cr);
  }
  return FALSE;
}

void dt_drawlayer_widgets_set_brush_profile_preview(dt_drawlayer_widgets_t *widgets,
                                                    const float opacity,
                                                    const float hardness,
                                                    const float sprinkles,
                                                    const float sprinkle_size,
                                                    const float sprinkle_coarseness,
                                                    const int selected_shape)
{
  if(IS_NULL_PTR(widgets)) return;

  const float new_opacity = _clamp01(opacity);
  const float new_hardness = _clamp01(hardness);
  const float new_sprinkles = _clamp01(sprinkles);
  const float new_size = fmaxf(1.0f, sprinkle_size);
  const float new_coarseness = _clamp01(sprinkle_coarseness);
  const int new_shape = CLAMP(selected_shape, DT_DRAWLAYER_BRUSH_SHAPE_LINEAR, DT_DRAWLAYER_BRUSH_SHAPE_SIGMOIDAL);

  if(fabsf(widgets->profile_opacity - new_opacity) > 1e-6f
     || fabsf(widgets->profile_hardness - new_hardness) > 1e-6f
     || fabsf(widgets->profile_sprinkles - new_sprinkles) > 1e-6f
     || fabsf(widgets->profile_sprinkle_size - new_size) > 1e-6f
     || fabsf(widgets->profile_sprinkle_coarseness - new_coarseness) > 1e-6f
     || widgets->profile_selected_shape != new_shape)
    widgets->profile_surface_dirty = TRUE;

  widgets->profile_opacity = new_opacity;
  widgets->profile_hardness = new_hardness;
  widgets->profile_sprinkles = new_sprinkles;
  widgets->profile_sprinkle_size = new_size;
  widgets->profile_sprinkle_coarseness = new_coarseness;
  widgets->profile_selected_shape = new_shape;
}

int dt_drawlayer_widgets_get_brush_profile_selection(const dt_drawlayer_widgets_t *widgets)
{
  return widgets ? widgets->profile_selected_shape : DT_DRAWLAYER_BRUSH_SHAPE_LINEAR;
}

gboolean dt_drawlayer_widgets_draw_brush_profiles(dt_drawlayer_widgets_t *widgets, GtkWidget *widget,
                                                  cairo_t *cr, const double pixels_per_dip)
{
  if(!widgets || !widget || IS_NULL_PTR(cr)) return FALSE;

  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  if(width <= 0 || height <= 0) return FALSE;

  const double ppd = (pixels_per_dip > 0.0) ? pixels_per_dip : 1.0;
  const int width_px = MAX(1, (int)ceil(width * ppd));
  const int height_px = MAX(1, (int)ceil(height * ppd));
  const gboolean size_changed = !widgets->profile_surface || widgets->profile_surface_width != width_px
                                || widgets->profile_surface_height != height_px
                                || fabs(widgets->profile_surface_ppd - ppd) > 1e-9;
  if(size_changed)
  {
    _clear_profile_surface(widgets);
    widgets->profile_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_px, height_px);
    if(cairo_surface_status(widgets->profile_surface) != CAIRO_STATUS_SUCCESS)
    {
      _clear_profile_surface(widgets);
      return FALSE;
    }
    cairo_surface_set_device_scale(widgets->profile_surface, ppd, ppd);
    widgets->profile_surface_width = width_px;
    widgets->profile_surface_height = height_px;
    widgets->profile_surface_ppd = ppd;
    widgets->profile_surface_dirty = TRUE;
  }

  float gap = 0.0f;
  int count = 0;
  _brush_profile_geometry(widget, NULL, NULL, NULL, NULL, &gap, &count);

  if(widgets->profile_surface_dirty)
  {
    unsigned char *data = cairo_image_surface_get_data(widgets->profile_surface);
    const int stride = cairo_image_surface_get_stride(widgets->profile_surface);
    memset(data, 0, (size_t)stride * height_px);
    int max_cell_w_px = 0;
    int max_cell_h_px = 0;

    for(int i = 0; i < count; i++)
    {
      float cell_x = 0.0f, cell_y = 0.0f, cell_w = 0.0f, cell_h = 0.0f;
      if(!_brush_profile_cell_rect(widget, i, &cell_x, &cell_y, &cell_w, &cell_h)) continue;
      const int x0 = MAX(0, (int)floor(cell_x * ppd));
      const int y0 = MAX(0, (int)floor(cell_y * ppd));
      const int x1 = MIN(width_px, (int)ceil((cell_x + cell_w) * ppd));
      const int y1 = MIN(height_px, (int)ceil((cell_y + cell_h) * ppd));
      max_cell_w_px = MAX(max_cell_w_px, MAX(1, x1 - x0));
      max_cell_h_px = MAX(max_cell_h_px, MAX(1, y1 - y0));
    }

    unsigned char *cell = g_malloc0((size_t)max_cell_w_px * max_cell_h_px * 4);
    float *rgba_scratch = g_malloc((size_t)max_cell_w_px * max_cell_h_px * 4 * sizeof(float));

    for(int i = 0; i < count; i++)
    {
      float cell_x = 0.0f, cell_y = 0.0f, cell_w = 0.0f, cell_h = 0.0f;
      if(!_brush_profile_cell_rect(widget, i, &cell_x, &cell_y, &cell_w, &cell_h)) continue;

      const int x0 = MAX(0, (int)floor(cell_x * ppd));
      const int y0 = MAX(0, (int)floor(cell_y * ppd));
      const int x1 = MIN(width_px, (int)ceil((cell_x + cell_w) * ppd));
      const int y1 = MIN(height_px, (int)ceil((cell_y + cell_h) * ppd));
      const int cell_w_px = MAX(1, x1 - x0);
      const int cell_h_px = MAX(1, y1 - y0);

      memset(cell, 0, (size_t)max_cell_w_px * max_cell_h_px * 4);
      _render_brush_profile_cell(cell, cell_w_px * 4, cell_w_px, cell_h_px, rgba_scratch, widgets, i);

      for(int py = 0; py < cell_h_px; py++)
      {
        memcpy(data + (size_t)(y0 + py) * stride + 4 * x0,
               cell + (size_t)py * cell_w_px * 4,
               (size_t)cell_w_px * 4);
      }
    }
    dt_free(rgba_scratch);
    dt_free(cell);

    cairo_surface_mark_dirty(widgets->profile_surface);
    widgets->profile_surface_dirty = FALSE;
  }

  cairo_set_source_surface(cr, widgets->profile_surface, 0.0, 0.0);
  cairo_paint(cr);

  for(int i = 0; i < count; i++)
  {
    float cell_x = 0.0f, cell_y = 0.0f, cell_w = 0.0f, cell_h = 0.0f;
    if(!_brush_profile_cell_rect(widget, i, &cell_x, &cell_y, &cell_w, &cell_h)) continue;

    const gboolean selected = (i == widgets->profile_selected_shape);
    cairo_rectangle(cr, cell_x, cell_y, cell_w, cell_h);
    cairo_set_line_width(cr, selected ? 2.5 : 1.0);
    if(selected) cairo_set_source_rgb(cr, 0.12, 0.45, 0.85);
    else cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.18);
    cairo_stroke(cr);
  }
  return FALSE;
}

gboolean dt_drawlayer_widgets_pick_brush_profile(dt_drawlayer_widgets_t *widgets, GtkWidget *widget,
                                                 const float x, const float y, int *shape)
{
  if(!widgets || !widget) return FALSE;

  for(int i = DT_DRAWLAYER_BRUSH_SHAPE_LINEAR; i <= DT_DRAWLAYER_BRUSH_SHAPE_SIGMOIDAL; i++)
  {
    float cell_x = 0.0f, cell_y = 0.0f, cell_w = 0.0f, cell_h = 0.0f;
    if(!_brush_profile_cell_rect(widget, i, &cell_x, &cell_y, &cell_w, &cell_h)) continue;
    if(x >= cell_x && x <= cell_x + cell_w && y >= cell_y && y <= cell_y + cell_h)
    {
      widgets->profile_selected_shape = i;
      widgets->profile_surface_dirty = TRUE;
      if(shape) *shape = i;
      return TRUE;
    }
  }
  return FALSE;
}
