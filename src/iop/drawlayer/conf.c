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

/*
 * drawlayer config subsystem (included directly by drawlayer.c)
 *
 * This file intentionally has no public header/API.
 */

/** @file
 *  @brief Drawlayer configuration read/write helpers (private include unit).
 */

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
static void _ensure_gui_conf_defaults(void)
{
  if(!dt_conf_key_exists(DRAWLAYER_CONF_BRUSH_SHAPE)) dt_conf_set_int(DRAWLAYER_CONF_BRUSH_SHAPE, DT_DRAWLAYER_BRUSH_SHAPE_LINEAR);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_BRUSH_MODE)) dt_conf_set_int(DRAWLAYER_CONF_BRUSH_MODE, DT_DRAWLAYER_BRUSH_MODE_PAINT);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_COLOR_R)) dt_conf_set_float(DRAWLAYER_CONF_COLOR_R, 1.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_COLOR_G)) dt_conf_set_float(DRAWLAYER_CONF_COLOR_G, 1.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_COLOR_B)) dt_conf_set_float(DRAWLAYER_CONF_COLOR_B, 1.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SOFTNESS)) dt_conf_set_float(DRAWLAYER_CONF_SOFTNESS, 0.5f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_OPACITY)) dt_conf_set_float(DRAWLAYER_CONF_OPACITY, 100.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_FLOW)) dt_conf_set_float(DRAWLAYER_CONF_FLOW, 100.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SPRINKLES)) dt_conf_set_float(DRAWLAYER_CONF_SPRINKLES, 0.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SPRINKLE_SIZE)) dt_conf_set_float(DRAWLAYER_CONF_SPRINKLE_SIZE, 3.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SPRINKLE_COARSENESS)) dt_conf_set_float(DRAWLAYER_CONF_SPRINKLE_COARSENESS, 50.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_DISTANCE)) dt_conf_set_float(DRAWLAYER_CONF_DISTANCE, 0.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SMOOTHING)) dt_conf_set_float(DRAWLAYER_CONF_SMOOTHING, 0.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SIZE)) dt_conf_set_float(DRAWLAYER_CONF_SIZE, 64.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_PICK_SOURCE)) dt_conf_set_int(DRAWLAYER_CONF_PICK_SOURCE, DRAWLAYER_PICK_SOURCE_INPUT);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_HDR_EV)) dt_conf_set_float(DRAWLAYER_CONF_HDR_EV, 0.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_PRESSURE_SIZE)) dt_conf_set_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE, FALSE);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY)) dt_conf_set_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY, FALSE);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_PRESSURE_FLOW)) dt_conf_set_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW, FALSE);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS)) dt_conf_set_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS, FALSE);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_TILT_SIZE)) dt_conf_set_bool(DRAWLAYER_CONF_MAP_TILT_SIZE, FALSE);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_TILT_OPACITY)) dt_conf_set_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY, FALSE);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_TILT_FLOW)) dt_conf_set_bool(DRAWLAYER_CONF_MAP_TILT_FLOW, FALSE);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_TILT_SOFTNESS)) dt_conf_set_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS, FALSE);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_ACCEL_SIZE))
  {
    const gboolean migrated = dt_conf_key_exists(DRAWLAYER_CONF_BASE "map_speed_size")
                                  ? dt_conf_get_bool(DRAWLAYER_CONF_BASE "map_speed_size")
                                  : FALSE;
    dt_conf_set_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE, migrated);
  }
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_ACCEL_OPACITY))
  {
    const gboolean migrated = dt_conf_key_exists(DRAWLAYER_CONF_BASE "map_speed_opacity")
                                  ? dt_conf_get_bool(DRAWLAYER_CONF_BASE "map_speed_opacity")
                                  : FALSE;
    dt_conf_set_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY, migrated);
  }
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_ACCEL_FLOW))
  {
    const gboolean migrated = dt_conf_key_exists(DRAWLAYER_CONF_BASE "map_speed_flow")
                                  ? dt_conf_get_bool(DRAWLAYER_CONF_BASE "map_speed_flow")
                                  : FALSE;
    dt_conf_set_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW, migrated);
  }
  if(!dt_conf_key_exists(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS))
  {
    const gboolean migrated = dt_conf_key_exists(DRAWLAYER_CONF_BASE "map_speed_softness")
                                  ? dt_conf_get_bool(DRAWLAYER_CONF_BASE "map_speed_softness")
                                  : FALSE;
    dt_conf_set_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS, migrated);
  }
  if(!dt_conf_key_exists(DRAWLAYER_CONF_PRESSURE_PROFILE)) dt_conf_set_int(DRAWLAYER_CONF_PRESSURE_PROFILE, DRAWLAYER_PROFILE_LINEAR);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_TILT_PROFILE)) dt_conf_set_int(DRAWLAYER_CONF_TILT_PROFILE, DRAWLAYER_PROFILE_LINEAR);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_ACCEL_PROFILE)) dt_conf_set_int(DRAWLAYER_CONF_ACCEL_PROFILE, DRAWLAYER_PROFILE_LINEAR);
}

/** @brief Read and clamp configured brush shape. */
static dt_iop_drawlayer_brush_shape_t _conf_brush_shape(void)
{
  return (dt_iop_drawlayer_brush_shape_t)CLAMP(dt_conf_get_int(DRAWLAYER_CONF_BRUSH_SHAPE),
                                               DT_DRAWLAYER_BRUSH_SHAPE_LINEAR, DT_DRAWLAYER_BRUSH_SHAPE_SIGMOIDAL);
}

/** @brief Read and clamp configured brush blend mode. */
static dt_iop_drawlayer_brush_mode_t _conf_brush_mode(void)
{
  return (dt_iop_drawlayer_brush_mode_t)CLAMP(dt_conf_get_int(DRAWLAYER_CONF_BRUSH_MODE),
                                              DT_DRAWLAYER_BRUSH_MODE_PAINT, DT_DRAWLAYER_BRUSH_MODE_SMUDGE);
}

/** @brief Read and clamp configured brush size (px). */
static float _conf_size(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SIZE), 1.0f, 2048.0f);
}

/** @brief Read and clamp configured stroke opacity (%). */
static float _conf_opacity(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_OPACITY), 0.0f, 100.0f);
}

/** @brief Read and clamp configured flow (%). */
static float _conf_flow(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_FLOW), 0.0f, 100.0f);
}

/** @brief Read and clamp configured sprinkles amount (%). */
static float _conf_sprinkles(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SPRINKLES), 0.0f, 100.0f);
}

/** @brief Read and clamp configured sprinkle feature size (px). */
static float _conf_sprinkle_size(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SPRINKLE_SIZE), 1.0f, 256.0f);
}

/** @brief Read and clamp configured sprinkle octave mix (%). */
static float _conf_sprinkle_coarseness(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SPRINKLE_COARSENESS), 0.0f, 100.0f);
}

/** @brief Read and clamp configured distance/sampling parameter (%). */
static float _conf_distance(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_DISTANCE), 0.0f, 100.0f);
}

/** @brief Read and clamp configured smoothing parameter (%). */
static float _conf_smoothing(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SMOOTHING), 0.0f, 100.0f);
}

/** @brief Read configured softness in [0,1]. */
static float _conf_softness(void)
{
  return _clamp01(dt_conf_get_float(DRAWLAYER_CONF_SOFTNESS));
}

/** @brief Derive hardness as complementary value of softness. */
static float _conf_hardness(void)
{
  return 1.0f - _conf_softness();
}

/** @brief Read and clamp HDR picker exposure compensation (EV). */
static float _conf_hdr_exposure(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_HDR_EV), 0.0f, 4.0f);
}

/** @brief Read and clamp color picker source selector. */
static drawlayer_pick_source_t _conf_pick_source(void)
{
  return (drawlayer_pick_source_t)CLAMP(dt_conf_get_int(DRAWLAYER_CONF_PICK_SOURCE),
                                        DRAWLAYER_PICK_SOURCE_INPUT, DRAWLAYER_PICK_SOURCE_OUTPUT);
}

/** @brief Read and clamp one mapping-profile enum key. */
static drawlayer_mapping_profile_t _conf_mapping_profile(const char *key)
{
  return (drawlayer_mapping_profile_t)CLAMP(dt_conf_get_int(key), DRAWLAYER_PROFILE_LINEAR,
                                            DRAWLAYER_PROFILE_INV_QUADRATIC);
}

/** @brief Read configured display RGB brush color. */
static void _conf_display_color(float rgb[3])
{
  rgb[0] = _clamp01(dt_conf_get_float(DRAWLAYER_CONF_COLOR_R));
  rgb[1] = _clamp01(dt_conf_get_float(DRAWLAYER_CONF_COLOR_G));
  rgb[2] = _clamp01(dt_conf_get_float(DRAWLAYER_CONF_COLOR_B));
}

/** @brief Build config key for one color-history channel entry. */
static void _color_history_key(const int index, const char channel, char *key, const size_t key_size)
{
  g_snprintf(key, key_size, DRAWLAYER_CONF_BASE "color_history_%d_%c", index, channel);
}

/** @brief Build config key for one color-history validity entry. */
static void _color_history_valid_key(const int index, char *key, const size_t key_size)
{
  g_snprintf(key, key_size, DRAWLAYER_CONF_BASE "color_history_%d_valid", index);
}

/** @brief Load persisted color-history stack from config into widgets state. */
static void _load_color_history(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->widgets) return;

  float history[DT_DRAWLAYER_COLOR_HISTORY_COUNT][3] = { { 0.0f } };
  gboolean valid[DT_DRAWLAYER_COLOR_HISTORY_COUNT] = { FALSE };

  char key[128] = { 0 };
  for(int i = 0; i < DT_DRAWLAYER_COLOR_HISTORY_COUNT; i++)
  {
    _color_history_valid_key(i, key, sizeof(key));
    valid[i] = dt_conf_key_exists(key) ? dt_conf_get_bool(key) : FALSE;

    _color_history_key(i, 'r', key, sizeof(key));
    history[i][0] = _clamp01(dt_conf_key_exists(key) ? dt_conf_get_float(key) : 0.0f);
    _color_history_key(i, 'g', key, sizeof(key));
    history[i][1] = _clamp01(dt_conf_key_exists(key) ? dt_conf_get_float(key) : 0.0f);
    _color_history_key(i, 'b', key, sizeof(key));
    history[i][2] = _clamp01(dt_conf_key_exists(key) ? dt_conf_get_float(key) : 0.0f);
  }
  dt_drawlayer_widgets_set_color_history(g->widgets, history, valid);
}

/** @brief Persist current widget color-history stack into config. */
static void _store_color_history(const dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->widgets) return;

  float history[DT_DRAWLAYER_COLOR_HISTORY_COUNT][3] = { { 0.0f } };
  gboolean valid[DT_DRAWLAYER_COLOR_HISTORY_COUNT] = { FALSE };
  dt_drawlayer_widgets_get_color_history(g->widgets, history, valid);

  char key[128] = { 0 };
  for(int i = 0; i < DT_DRAWLAYER_COLOR_HISTORY_COUNT; i++)
  {
    _color_history_valid_key(i, key, sizeof(key));
    dt_conf_set_bool(key, valid[i]);

    _color_history_key(i, 'r', key, sizeof(key));
    dt_conf_set_float(key, history[i][0]);
    _color_history_key(i, 'g', key, sizeof(key));
    dt_conf_set_float(key, history[i][1]);
    _color_history_key(i, 'b', key, sizeof(key));
    dt_conf_set_float(key, history[i][2]);
  }
}

/** @brief Push current display color to history and trigger swatch redraw. */
static void _remember_display_color(dt_iop_module_t *self, const float display_rgb[3])
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets || !display_rgb) return;

  if(!dt_drawlayer_widgets_push_color_history(g->widgets, display_rgb)) return;
  _store_color_history(g);
  if(g->color_swatch) gtk_widget_queue_draw(g->color_swatch);
}

/** @brief Apply display-space brush color to conf, widgets and redraw. */
static void _apply_display_brush_color(dt_iop_module_t *self, const float display_rgb[3], const gboolean remember)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !g || !g->widgets || !display_rgb) return;

  dt_conf_set_float(DRAWLAYER_CONF_COLOR_R, _clamp01(display_rgb[0]));
  dt_conf_set_float(DRAWLAYER_CONF_COLOR_G, _clamp01(display_rgb[1]));
  dt_conf_set_float(DRAWLAYER_CONF_COLOR_B, _clamp01(display_rgb[2]));

  dt_drawlayer_widgets_set_display_color(g->widgets, display_rgb);
  _clear_cursor_stamp_surface(g);

  if(remember) _remember_display_color(self, display_rgb);

  if(g->color) gtk_widget_queue_draw(g->color);
  if(g->color_swatch) gtk_widget_queue_draw(g->color_swatch);
  dt_control_queue_redraw_center();
}

/** @brief Refresh picker widgets from persisted config color. */
static void _sync_color_picker_from_conf(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets) return;

  float display_rgb[3] = { 0.0f };
  _conf_display_color(display_rgb);

  dt_drawlayer_widgets_set_display_color(g->widgets, display_rgb);
  if(g->color_swatch) gtk_widget_queue_draw(g->color_swatch);
}

/** @brief Sync active GUI widget values back into persistent config keys. */
static void _sync_params_from_gui(dt_iop_module_t *self, const gboolean record_history)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  (void)record_history;
  if(!g || (darktable.gui && darktable.gui->reset)) return;

  dt_conf_set_int(DRAWLAYER_CONF_BRUSH_SHAPE, dt_drawlayer_widgets_get_brush_profile_selection(g->widgets));
  dt_conf_set_int(DRAWLAYER_CONF_BRUSH_MODE, dt_bauhaus_combobox_get(g->brush_mode));
  dt_conf_set_float(DRAWLAYER_CONF_SIZE, dt_bauhaus_slider_get(g->size));
  dt_conf_set_float(DRAWLAYER_CONF_DISTANCE, dt_bauhaus_slider_get(g->distance));
  dt_conf_set_float(DRAWLAYER_CONF_SMOOTHING, dt_bauhaus_slider_get(g->smoothing));
  dt_conf_set_float(DRAWLAYER_CONF_OPACITY, dt_bauhaus_slider_get(g->opacity));
  dt_conf_set_float(DRAWLAYER_CONF_FLOW, dt_bauhaus_slider_get(g->flow));
  dt_conf_set_float(DRAWLAYER_CONF_SPRINKLES, dt_bauhaus_slider_get(g->sprinkles));
  dt_conf_set_float(DRAWLAYER_CONF_SPRINKLE_SIZE, dt_bauhaus_slider_get(g->sprinkle_size));
  dt_conf_set_float(DRAWLAYER_CONF_SPRINKLE_COARSENESS, dt_bauhaus_slider_get(g->sprinkle_coarseness));
  dt_conf_set_float(DRAWLAYER_CONF_SOFTNESS, 1.0f - dt_bauhaus_slider_get(g->softness));
  if(g->image_colorpicker_source)
    dt_conf_set_int(DRAWLAYER_CONF_PICK_SOURCE, dt_bauhaus_combobox_get(g->image_colorpicker_source));
  dt_conf_set_float(DRAWLAYER_CONF_HDR_EV, dt_bauhaus_slider_get(g->hdr_exposure));

  dt_conf_set_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_pressure_size)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_pressure_opacity)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_pressure_flow)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_pressure_softness)));

  dt_conf_set_bool(DRAWLAYER_CONF_MAP_TILT_SIZE, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_tilt_size)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_tilt_opacity)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_TILT_FLOW, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_tilt_flow)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_tilt_softness)));

  dt_conf_set_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_accel_size)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_accel_opacity)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_accel_flow)));
  dt_conf_set_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->map_accel_softness)));

  if(g->pressure_profile) dt_conf_set_int(DRAWLAYER_CONF_PRESSURE_PROFILE, dt_bauhaus_combobox_get(g->pressure_profile));
  if(g->tilt_profile) dt_conf_set_int(DRAWLAYER_CONF_TILT_PROFILE, dt_bauhaus_combobox_get(g->tilt_profile));
  if(g->accel_profile) dt_conf_set_int(DRAWLAYER_CONF_ACCEL_PROFILE, dt_bauhaus_combobox_get(g->accel_profile));
}
