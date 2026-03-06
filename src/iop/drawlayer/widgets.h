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

#pragma once

#include "iop/drawlayer/brush.h"

#include <gtk/gtk.h>

/** @file
 *  @brief Public color-picker/history widget API for drawlayer GUI.
 */

#define DT_DRAWLAYER_COLOR_PICKER_HEIGHT 184
#define DT_DRAWLAYER_COLOR_HISTORY_COUNT 10
#define DT_DRAWLAYER_COLOR_HISTORY_COLS 5
#define DT_DRAWLAYER_COLOR_HISTORY_ROWS 2
#define DT_DRAWLAYER_COLOR_HISTORY_HEIGHT 44

/** @brief Opaque widget runtime state. */
typedef struct dt_drawlayer_widgets_t dt_drawlayer_widgets_t;

/** @brief Allocate and initialize widget runtime state. */
dt_drawlayer_widgets_t *dt_drawlayer_widgets_init(void);
/** @brief Destroy widget runtime state and owned surfaces. */
void dt_drawlayer_widgets_cleanup(dt_drawlayer_widgets_t **widgets);

/** @brief Set current display RGB color and refresh picker state. */
void dt_drawlayer_widgets_set_display_color(dt_drawlayer_widgets_t *widgets, const float display_rgb[3]);
/** @brief Get current display RGB color. */
gboolean dt_drawlayer_widgets_get_display_color(const dt_drawlayer_widgets_t *widgets, float display_rgb[3]);

/** @brief Mark picker backing surface dirty for redraw/rebuild. */
void dt_drawlayer_widgets_mark_picker_dirty(dt_drawlayer_widgets_t *widgets);

/** @brief Replace full color-history stack and validity flags. */
void dt_drawlayer_widgets_set_color_history(dt_drawlayer_widgets_t *widgets,
                                            const float history[DT_DRAWLAYER_COLOR_HISTORY_COUNT][3],
                                            const gboolean valid[DT_DRAWLAYER_COLOR_HISTORY_COUNT]);
/** @brief Read full color-history stack and validity flags. */
void dt_drawlayer_widgets_get_color_history(const dt_drawlayer_widgets_t *widgets,
                                            float history[DT_DRAWLAYER_COLOR_HISTORY_COUNT][3],
                                            gboolean valid[DT_DRAWLAYER_COLOR_HISTORY_COUNT]);
/** @brief Push one color in history if it differs from current head. */
gboolean dt_drawlayer_widgets_push_color_history(dt_drawlayer_widgets_t *widgets, const float display_rgb[3]);

/** @brief Update picker selection from widget coordinates during drag. */
gboolean dt_drawlayer_widgets_update_from_picker_position(dt_drawlayer_widgets_t *widgets, GtkWidget *widget,
                                                          float x, float y, float display_rgb[3]);
/** @brief End picker drag and return final selected color. */
gboolean dt_drawlayer_widgets_finish_picker_drag(dt_drawlayer_widgets_t *widgets, float display_rgb[3]);
/** @brief Query whether picker drag is currently active. */
gboolean dt_drawlayer_widgets_is_picker_dragging(const dt_drawlayer_widgets_t *widgets);

/** @brief Hit-test and pick color from history swatches. */
gboolean dt_drawlayer_widgets_pick_history_color(const dt_drawlayer_widgets_t *widgets, GtkWidget *widget,
                                                 float x, float y, float display_rgb[3]);

/** @brief Draw full picker UI (map, marker, history swatches). */
gboolean dt_drawlayer_widgets_draw_picker(dt_drawlayer_widgets_t *widgets, GtkWidget *widget, cairo_t *cr,
                                          double pixels_per_dip);
/** @brief Draw compact current-color swatch widget. */
gboolean dt_drawlayer_widgets_draw_swatch(const dt_drawlayer_widgets_t *widgets, GtkWidget *widget, cairo_t *cr);

/** @brief Update cached brush-profile preview parameters and selected profile. */
void dt_drawlayer_widgets_set_brush_profile_preview(dt_drawlayer_widgets_t *widgets,
                                                    float opacity,
                                                    float hardness,
                                                    float sprinkles,
                                                    float sprinkle_size,
                                                    float sprinkle_coarseness,
                                                    int selected_shape);
/** @brief Read currently selected brush profile. */
int dt_drawlayer_widgets_get_brush_profile_selection(const dt_drawlayer_widgets_t *widgets);
/** @brief Draw selectable row of brush-profile previews. */
gboolean dt_drawlayer_widgets_draw_brush_profiles(dt_drawlayer_widgets_t *widgets, GtkWidget *widget,
                                                  cairo_t *cr, double pixels_per_dip);
/** @brief Hit-test and select one brush profile from the preview row. */
gboolean dt_drawlayer_widgets_pick_brush_profile(dt_drawlayer_widgets_t *widgets, GtkWidget *widget,
                                                 float x, float y, int *shape);
