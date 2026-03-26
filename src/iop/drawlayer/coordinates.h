#pragma once

#include "iop/drawlayer/common.h"
#include "iop/drawlayer/cache.h"

/** @file
 *  @brief Shared coordinate transforms and geometry computations for drawlayer.
 */

typedef struct drawlayer_view_patch_t
{
  int x;
  int y;
  int width;
  int height;
} drawlayer_view_patch_t;

typedef struct drawlayer_view_patch_info_t
{
  drawlayer_view_patch_t patch;
  float layer_x0;
  float layer_y0;
  float layer_x1;
  float layer_y1;
} drawlayer_view_patch_info_t;

gboolean dt_drawlayer_widget_points_to_layer_coords(dt_iop_module_t *self, float *pts, int count);
gboolean dt_drawlayer_layer_points_to_widget_coords(dt_iop_module_t *self, float *pts, int count);
gboolean dt_drawlayer_widget_to_layer_coords(dt_iop_module_t *self, double wx, double wy, float *lx, float *ly);
gboolean dt_drawlayer_layer_to_widget_coords(dt_iop_module_t *self, float x, float y, float *wx, float *wy);
gboolean dt_drawlayer_layer_bounds_to_widget_bounds(dt_iop_module_t *self, float x0, float y0,
                                                    float x1, float y1,
                                                    float *left, float *top,
                                                    float *right, float *bottom);
float dt_drawlayer_widget_brush_radius(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab, float fallback);
float dt_drawlayer_current_live_padding(dt_iop_module_t *self);
gboolean dt_drawlayer_compute_view_patch(dt_iop_module_t *self, float padding, drawlayer_view_patch_info_t *view);
