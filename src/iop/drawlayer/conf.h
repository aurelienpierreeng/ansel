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

/** @file
 *  @brief The brush settings the drawlayer module keeps in the application configuration.
 *
 *  Every read here parses a string -- dt_conf_get_float() runs the expression calculator --
 *  and takes the application-wide conf mutex, so these are called when something CHANGES the
 *  brush, never per pointer motion. The GUI caches what it needs in its own brush_settings.
 */

#ifndef DT_IOP_DRAWLAYER_CONF_H
#define DT_IOP_DRAWLAYER_CONF_H

#include "iop/drawlayer/brush.h"       // dt_iop_drawlayer_brush_shape_t, ..._brush_mode_t
#include "iop/drawlayer/common.h"      // dt_iop_drawlayer_params_t, dt_iop_drawlayer_gui_data_t
#include "iop/drawlayer/module.h"      // drawlayer_pick_source_t, drawlayer_mapping_profile_t
#include "develop/imageop.h"           // dt_iop_module_t

/* The configuration keys themselves. They are the module's published vocabulary for its
 * brush: the GUI writes them, these accessors read them, and the tests name them. */
#define DRAWLAYER_CONF_BASE "plugins/drawlayer/"
#define DRAWLAYER_CONF_BRUSH_SHAPE DRAWLAYER_CONF_BASE "brush_shape"
#define DRAWLAYER_CONF_BRUSH_MODE DRAWLAYER_CONF_BASE "brush_mode"
#define DRAWLAYER_CONF_COLOR_R DRAWLAYER_CONF_BASE "color_r"
#define DRAWLAYER_CONF_COLOR_G DRAWLAYER_CONF_BASE "color_g"
#define DRAWLAYER_CONF_COLOR_B DRAWLAYER_CONF_BASE "color_b"
#define DRAWLAYER_CONF_SOFTNESS DRAWLAYER_CONF_BASE "softness"
#define DRAWLAYER_CONF_OPACITY DRAWLAYER_CONF_BASE "opacity"
#define DRAWLAYER_CONF_FLOW DRAWLAYER_CONF_BASE "flow"
#define DRAWLAYER_CONF_SPRINKLES DRAWLAYER_CONF_BASE "sprinkles"
#define DRAWLAYER_CONF_SPRINKLE_SIZE DRAWLAYER_CONF_BASE "sprinkle_size"
#define DRAWLAYER_CONF_SPRINKLE_COARSENESS DRAWLAYER_CONF_BASE "sprinkle_coarseness"
#define DRAWLAYER_CONF_DISTANCE DRAWLAYER_CONF_BASE "distance"
#define DRAWLAYER_CONF_SMOOTHING DRAWLAYER_CONF_BASE "smoothing"
#define DRAWLAYER_CONF_SIZE DRAWLAYER_CONF_BASE "size"
#define DRAWLAYER_CONF_PICK_SOURCE DRAWLAYER_CONF_BASE "pick_source"
#define DRAWLAYER_CONF_HDR_EV DRAWLAYER_CONF_BASE "hdr_exposure"
#define DRAWLAYER_CONF_MAP_PRESSURE_SIZE DRAWLAYER_CONF_BASE "map_pressure_size"
#define DRAWLAYER_CONF_MAP_PRESSURE_OPACITY DRAWLAYER_CONF_BASE "map_pressure_opacity"
#define DRAWLAYER_CONF_MAP_PRESSURE_FLOW DRAWLAYER_CONF_BASE "map_pressure_flow"
#define DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS DRAWLAYER_CONF_BASE "map_pressure_softness"
#define DRAWLAYER_CONF_MAP_TILT_SIZE DRAWLAYER_CONF_BASE "map_tilt_size"
#define DRAWLAYER_CONF_MAP_TILT_OPACITY DRAWLAYER_CONF_BASE "map_tilt_opacity"
#define DRAWLAYER_CONF_MAP_TILT_FLOW DRAWLAYER_CONF_BASE "map_tilt_flow"
#define DRAWLAYER_CONF_MAP_TILT_SOFTNESS DRAWLAYER_CONF_BASE "map_tilt_softness"
#define DRAWLAYER_CONF_MAP_ACCEL_SIZE DRAWLAYER_CONF_BASE "map_acceleration_size"
#define DRAWLAYER_CONF_MAP_ACCEL_OPACITY DRAWLAYER_CONF_BASE "map_acceleration_opacity"
#define DRAWLAYER_CONF_MAP_ACCEL_FLOW DRAWLAYER_CONF_BASE "map_acceleration_flow"
#define DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS DRAWLAYER_CONF_BASE "map_acceleration_hardness"
#define DRAWLAYER_CONF_PRESSURE_PROFILE DRAWLAYER_CONF_BASE "pressure_profile"
#define DRAWLAYER_CONF_TILT_PROFILE DRAWLAYER_CONF_BASE "tilt_profile"
#define DRAWLAYER_CONF_ACCEL_PROFILE DRAWLAYER_CONF_BASE "acceleration_profile"

/** @brief Ensure all drawlayer GUI config keys exist with sane defaults. */
void dt_drawlayer_conf_ensure_defaults(void);

/** @brief Read and clamp configured brush shape. */
dt_iop_drawlayer_brush_shape_t dt_drawlayer_conf_brush_shape(void);

/** @brief Read and clamp configured brush blend mode. */
dt_iop_drawlayer_brush_mode_t dt_drawlayer_conf_brush_mode(void);

/** @brief Read and clamp configured brush size (px). */
float dt_drawlayer_conf_size(void);

/** @brief Read and clamp configured stroke opacity (%). */
float dt_drawlayer_conf_opacity(void);

/** @brief Read and clamp configured flow (%). */
float dt_drawlayer_conf_flow(void);

/** @brief Read and clamp configured sprinkles amount (%). */
float dt_drawlayer_conf_sprinkles(void);

/** @brief Read and clamp configured sprinkle feature size (px). */
float dt_drawlayer_conf_sprinkle_size(void);

/** @brief Read and clamp configured sprinkle octave mix (%). */
float dt_drawlayer_conf_sprinkle_coarseness(void);

/** @brief Read and clamp configured distance/sampling parameter (%). */
float dt_drawlayer_conf_distance(void);

/** @brief Read and clamp configured smoothing parameter (%). */
float dt_drawlayer_conf_smoothing(void);

/** @brief Derive hardness as complementary value of softness. */
float dt_drawlayer_conf_hardness(void);

/** @brief Read and clamp HDR picker exposure compensation (EV). */
float dt_drawlayer_conf_hdr_exposure(void);

/** @brief Read and clamp color picker source selector. */
drawlayer_pick_source_t dt_drawlayer_conf_pick_source(void);

/** @brief Read and clamp one mapping-profile enum key. */
drawlayer_mapping_profile_t dt_drawlayer_conf_mapping_profile(const char *key);

/** @brief Read configured display RGB brush color. */
void dt_drawlayer_conf_display_color(float rgb[3]);

/** @brief Load persisted color-history stack from config into widgets state. */
void dt_drawlayer_conf_load_color_history(dt_iop_drawlayer_gui_data_t *g);

/** @brief Push current display color to history and trigger swatch redraw. */
void dt_drawlayer_conf_remember_display_color(dt_iop_module_t *self, const float display_rgb[3]);

/** @brief Apply display-space brush color to conf, widgets and redraw. */
void dt_drawlayer_conf_apply_display_brush_color(dt_iop_module_t *self, const float display_rgb[3], const gboolean remember);

/** @brief Refresh picker widgets from persisted config color. */
void dt_drawlayer_conf_sync_color_picker(dt_iop_module_t *self);

/** @brief Sync active GUI widget values back into persistent config keys. */
void dt_drawlayer_conf_sync_params_from_gui(dt_iop_module_t *self, const gboolean record_history);

#endif
