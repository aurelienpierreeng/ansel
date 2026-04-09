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
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/chromatic_adaptation.h"
#include "common/darktable.h"
#include "common/iop_profile.h"
#include "common/illuminants.h"
#include "common/matrices.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/channelmixerrgb_shared.h"
#include "iop/iop_api.h"

// Keep the shared implementation in this translation unit to avoid
// duplicate globals from a separate compiled object.
#include "channelmixerrgb_shared.c"

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_splittoning_rgb_params_t)

#define DT_SPLITTONING_RGB_POINT_COUNT 2
#define DT_SPLITTONING_RGB_ROW_COUNT 3
#define DT_SPLITTONING_RGB_COMPLETE_COUNT 3
#define DT_SPLITTONING_RGB_PREVIEW_HEIGHT 60
typedef enum dt_iop_splittoning_rgb_point_t
{
  DT_SPLITTONING_RGB_POINT_DARK = 0,
  DT_SPLITTONING_RGB_POINT_BRIGHT = 1,
} dt_iop_splittoning_rgb_point_t;

typedef enum dt_iop_splittoning_rgb_mixer_mode_t
{
  DT_SPLITTONING_RGB_MIXER_COMPLETE = 0,
  DT_SPLITTONING_RGB_MIXER_SIMPLE = 1,
  DT_SPLITTONING_RGB_MIXER_PRIMARIES = 2,
} dt_iop_splittoning_rgb_mixer_mode_t;

typedef struct dt_iop_splittoning_rgb_params_t
{
  float ev[DT_SPLITTONING_RGB_POINT_COUNT]; // $MIN: -16.0 $MAX: 16.0 $DEFAULT: 0.0 $DESCRIPTION: "EV"
  float temperature[DT_SPLITTONING_RGB_POINT_COUNT]; // $MIN: 1667.0 $MAX: 25000.0 $DEFAULT: 5003.0 $DESCRIPTION: "temperature"
  float red[DT_SPLITTONING_RGB_POINT_COUNT][DT_SPLITTONING_RGB_ROW_COUNT];
  float green[DT_SPLITTONING_RGB_POINT_COUNT][DT_SPLITTONING_RGB_ROW_COUNT];
  float blue[DT_SPLITTONING_RGB_POINT_COUNT][DT_SPLITTONING_RGB_ROW_COUNT];
  gboolean normalize[DT_SPLITTONING_RGB_POINT_COUNT][DT_SPLITTONING_RGB_ROW_COUNT];
} dt_iop_splittoning_rgb_params_t;

typedef struct dt_iop_splittoning_rgb_data_t
{
  dt_colormatrix_t point_matrix[DT_SPLITTONING_RGB_POINT_COUNT];
  dt_colormatrix_t rgb_to_xyz_transposed;
  float point_matrix_cl[DT_SPLITTONING_RGB_POINT_COUNT][12];
  float rgb_to_xyz_cl[12];
  float dark_luminance;
  float bright_luminance;
} dt_iop_splittoning_rgb_data_t;

typedef struct dt_iop_splittoning_rgb_global_data_t
{
  int kernel_splittoningrgb;
} dt_iop_splittoning_rgb_global_data_t;

typedef struct dt_iop_splittoning_rgb_point_gui_t
{
  GtkWidget *page;
  GtkWidget *ev;
  GtkWidget *temperature;
  GtkWidget *mixer_mode;
  GtkWidget *mixer_stack;
  GtkWidget *complete[DT_SPLITTONING_RGB_COMPLETE_COUNT][DT_SPLITTONING_RGB_COMPLETE_COUNT];
  GtkWidget *normalize[DT_SPLITTONING_RGB_ROW_COUNT];
  GtkWidget *simple_theta;
  GtkWidget *simple_psi;
  GtkWidget *simple_stretch_1;
  GtkWidget *simple_stretch_2;
  GtkWidget *simple_coupling_1;
  GtkWidget *simple_coupling_2;
  GtkWidget *primaries_achromatic_hue;
  GtkWidget *primaries_achromatic_purity;
  GtkWidget *primaries_red_hue;
  GtkWidget *primaries_red_purity;
  GtkWidget *primaries_green_hue;
  GtkWidget *primaries_green_purity;
  GtkWidget *primaries_blue_hue;
  GtkWidget *primaries_blue_purity;
  GtkWidget *primaries_gain;
} dt_iop_splittoning_rgb_point_gui_t;

typedef struct dt_iop_splittoning_rgb_gui_data_t
{
  GtkWidget *preview;
  cairo_surface_t *preview_surface;
  int preview_width;
  int preview_height;
  GtkNotebook *tabs;
  dt_iop_splittoning_rgb_point_gui_t point[DT_SPLITTONING_RGB_POINT_COUNT];
} dt_iop_splittoning_rgb_gui_data_t;

static const char *const _mode_conf[DT_SPLITTONING_RGB_POINT_COUNT] = {
  "plugins/darkroom/splittoningrgb/dark_mixer_mode",
  "plugins/darkroom/splittoningrgb/bright_mixer_mode"
};

static const char *const _point_label[DT_SPLITTONING_RGB_POINT_COUNT] = {
  N_("dark"),
  N_("bright")
};

static void _update_point_slider_colors(dt_iop_module_t *self, int point);

const char *name()
{
  return _("split-toning RGB");
}

const char *aliases()
{
  return _("split toning|split tone RGB");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("blend two CAT16 plus RGB mixer corrections across brightness keyframes"),
                                _("creative or corrective"), _("linear, RGB, scene-referred"),
                                _("linear, RGB"), _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  default_input_format(self, pipe, piece, dsc);
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
}

static inline float _ev_to_grey(const float ev)
{
  return exp2f(ev);
}

static inline float _ev_to_luminance(const float ev)
{
  return _ev_to_grey(ev);
}

static void _temperature_to_xy(const float temperature, float *x, float *y)
{
  if(temperature > 4000.f)
    CCT_to_xy_daylight(temperature, x, y);
  else
    CCT_to_xy_blackbody(temperature, x, y);
}

static void _get_point_rows(const dt_iop_splittoning_rgb_params_t *p, const int point, float rows[3][3],
                            gboolean normalize[3])
{
  for(int col = 0; col < 3; col++)
  {
    rows[0][col] = p->red[point][col];
    rows[1][col] = p->green[point][col];
    rows[2][col] = p->blue[point][col];
  }

  for(int row = 0; row < 3; row++) normalize[row] = p->normalize[point][row];
}

static void _set_point_rows(dt_iop_splittoning_rgb_params_t *p, const int point, const float M[3][3])
{
  for(int col = 0; col < 3; col++)
  {
    p->red[point][col] = M[0][col];
    p->green[point][col] = M[1][col];
    p->blue[point][col] = M[2][col];
  }
}

static void _set_point_complete_widgets(dt_iop_splittoning_rgb_gui_data_t *g, const dt_iop_splittoning_rgb_params_t *p,
                                        const int point)
{
  const float *const rows[3] = { p->red[point], p->green[point], p->blue[point] };

  for(int row = 0; row < 3; row++)
  {
    for(int col = 0; col < 3; col++) dt_bauhaus_slider_set(g->point[point].complete[row][col], rows[row][col]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->point[point].normalize[row]), p->normalize[point][row]);
  }
}

static void _set_point_mixer_mode(dt_iop_splittoning_rgb_gui_data_t *g, const int point,
                                  const dt_iop_splittoning_rgb_mixer_mode_t mode)
{
  gtk_stack_set_visible_child_name(GTK_STACK(g->point[point].mixer_stack),
                                   mode == DT_SPLITTONING_RGB_MIXER_SIMPLE ? "simple"
                                   : mode == DT_SPLITTONING_RGB_MIXER_PRIMARIES ? "primaries"
                                   : "complete");
  gtk_widget_queue_resize(g->point[point].mixer_stack);
  gtk_widget_queue_resize(g->point[point].page);
  gtk_widget_queue_resize(GTK_WIDGET(g->tabs));
}

static void _build_cat16_rgb_matrix(const dt_iop_order_iccprofile_info_t *work_profile, const float temperature,
                                    float CAT[3][3])
{
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      CAT[row][col] = row == col ? 1.f : 0.f;

  if(!work_profile) return;

  float x = 0.f;
  float y = 0.f;
  dt_aligned_pixel_t illuminant_XYZ = { 0.f };
  dt_aligned_pixel_t illuminant_LMS = { 0.f };

  _temperature_to_xy(temperature, &x, &y);
  illuminant_xy_to_XYZ(x, y, illuminant_XYZ);
  dt_store_simd_aligned(illuminant_LMS,
                        convert_any_XYZ_to_LMS(dt_load_simd_aligned(illuminant_XYZ), DT_ADAPTATION_CAT16));

  // Build the RGB adaptation by probing the three working-space basis vectors independently.
  for(int col = 0; col < 3; col++)
  {
    dt_aligned_pixel_t RGB_in = { 0.f };
    dt_aligned_pixel_t XYZ_in = { 0.f };
    dt_aligned_pixel_t XYZ_out = { 0.f };
    dt_aligned_pixel_t RGB_out = { 0.f };

    RGB_in[col] = 1.f;
    dt_apply_transposed_color_matrix(RGB_in, work_profile->matrix_in_transposed, XYZ_in);
    dt_store_simd_aligned(XYZ_out,
                          chroma_adapt_pixel(dt_load_simd_aligned(XYZ_in), dt_load_simd_aligned(illuminant_LMS),
                                             DT_ADAPTATION_CAT16, 1.f));
    dt_apply_transposed_color_matrix(XYZ_out, work_profile->matrix_out_transposed, RGB_out);

    for(int row = 0; row < 3; row++) CAT[row][col] = RGB_out[row];
  }
}

static int _build_point_transform(const dt_iop_splittoning_rgb_params_t *p, const int point,
                                  const dt_iop_order_iccprofile_info_t *work_profile, dt_colormatrix_t point_matrix)
{
  float rows[3][3] = { { 0.f } };
  gboolean normalize[3] = { FALSE, FALSE, FALSE };
  float mixer[3][3] = { { 0.f } };
  float CAT[3][3] = { { 0.f } };
  float combined[3][3] = { { 0.f } };

  _get_point_rows(p, point, rows, normalize);
  if(!dt_iop_channelmixer_shared_get_matrix(rows, normalize, FALSE, mixer)) return 1;

  _build_cat16_rgb_matrix(work_profile, p->temperature[point], CAT);
  dt_iop_channelmixer_shared_mul3x3(mixer, CAT, combined);

  memset(point_matrix, 0, sizeof(dt_colormatrix_t));
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      point_matrix[col][row] = combined[row][col];
  return 0;
}

/**
 * @brief Interpolate two keyed correction matrices entry-wise.
 *
 * The transforms are stored in transposed padded form so they can be fed
 * directly to the SIMD matrix-vector multiply. Interpolating them entry-wise in
 * that storage remains equivalent to interpolating the underlying 3x3 matrix.
 *
 * @param[in] from Source keyed correction matrix in transposed padded form.
 * @param[in] to Destination keyed correction matrix in transposed padded form.
 * @param[in] alpha Interpolation factor from source to destination.
 * @param[out] interpolated Interpolated matrix in the same transposed padded
 * form.
 */
static inline __attribute__((always_inline)) void _interpolate_matrix(const dt_colormatrix_t from,
                                                                      const dt_colormatrix_t to, const float alpha,
                                                                      dt_colormatrix_t interpolated)
{
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      interpolated[row][col] = from[row][col] + alpha * (to[row][col] - from[row][col]);

  interpolated[0][3] = 0.f;
  interpolated[1][3] = 0.f;
  interpolated[2][3] = 0.f;
}

/**
 * @brief Build the keyed full transform matrix for one scene luminance.
 *
 * Inside the [dark ; bright] segment, the module linearly interpolates between
 * the two user transforms. Outside that segment, the nearest transform fades
 * linearly back to identity over one segment length so the correction tapers
 * out instead of clipping.
 *
 * @param[in] luminance Current pixel scene luminance in XYZ Y.
 * @param[in] d Precomputed module state for the current pipe.
 * @param[out] matrix Keyed full transform matrix in transposed padded form.
 */
static inline __attribute__((always_inline)) void _get_split_matrix(const float luminance,
                                                                    const dt_iop_splittoning_rgb_data_t *d,
                                                                    dt_colormatrix_t matrix)
{
  dt_colormatrix_t identity = { { 1.f, 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f, 0.f } };
  const float segment = fmaxf(d->bright_luminance - d->dark_luminance, NORM_MIN);

  if(luminance <= d->dark_luminance)
  {
    const float alpha = CLAMP(1.f - (d->dark_luminance - fmaxf(luminance, 0.f)) / segment, 0.f, 1.f);
    _interpolate_matrix(identity, d->point_matrix[DT_SPLITTONING_RGB_POINT_DARK], alpha, matrix);
    return;
  }

  if(luminance >= d->bright_luminance)
  {
    const float alpha = CLAMP(1.f - (luminance - d->bright_luminance) / segment, 0.f, 1.f);
    _interpolate_matrix(identity, d->point_matrix[DT_SPLITTONING_RGB_POINT_BRIGHT], alpha, matrix);
    return;
  }

  const float alpha = CLAMP((luminance - d->dark_luminance) / segment, 0.f, 1.f);
  _interpolate_matrix(d->point_matrix[DT_SPLITTONING_RGB_POINT_DARK],
                      d->point_matrix[DT_SPLITTONING_RGB_POINT_BRIGHT], alpha, matrix);
}

static gboolean _sync_simple_from_params(dt_iop_module_t *self, const int point, float *error)
{
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  GtkWidget *const widgets[6]
      = { g->point[point].simple_theta, g->point[point].simple_psi, g->point[point].simple_stretch_1,
          g->point[point].simple_stretch_2, g->point[point].simple_coupling_1,
          g->point[point].simple_coupling_2 };
  float rows[3][3] = { { 0.f } };
  gboolean normalize[3] = { FALSE, FALSE, FALSE };
  float M[3][3] = { { 0.f } };
  float roundtrip[3][3] = { { 0.f } };
  dt_iop_channelmixer_shared_simple_params_t simple;

  if(error) *error = INFINITY;

  _get_point_rows(p, point, rows, normalize);
  if(!dt_iop_channelmixer_shared_rows_are_normalized(normalize)) return FALSE;
  if(!dt_iop_channelmixer_shared_get_matrix(rows, normalize, FALSE, M)) return FALSE;

  dt_iop_channelmixer_shared_simple_from_matrix(M, &simple);
  dt_iop_channelmixer_shared_simple_to_matrix(&simple, roundtrip);

  const float roundtrip_error = dt_iop_channelmixer_shared_roundtrip_error(M, roundtrip);
  if(error) *error = roundtrip_error;

  ++darktable.gui->reset;
  dt_iop_channelmixer_shared_simple_to_sliders(&simple, widgets);
  --darktable.gui->reset;

  return isfinite(roundtrip_error) && roundtrip_error <= DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS;
}

static gboolean _sync_primaries_from_params(dt_iop_module_t *self, const int point, float *error)
{
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  GtkWidget *const widgets[9]
      = { g->point[point].primaries_achromatic_hue, g->point[point].primaries_achromatic_purity,
          g->point[point].primaries_red_hue, g->point[point].primaries_red_purity,
          g->point[point].primaries_green_hue, g->point[point].primaries_green_purity,
          g->point[point].primaries_blue_hue, g->point[point].primaries_blue_purity,
          g->point[point].primaries_gain };
  float rows[3][3] = { { 0.f } };
  gboolean normalize[3] = { FALSE, FALSE, FALSE };
  float M[3][3] = { { 0.f } };
  float roundtrip[3][3] = { { 0.f } };
  dt_iop_channelmixer_shared_primaries_params_t primaries;

  if(error) *error = INFINITY;

  _get_point_rows(p, point, rows, normalize);
  if(!dt_iop_channelmixer_shared_get_matrix(rows, normalize, FALSE, M)) return FALSE;
  if(!dt_iop_channelmixer_shared_primaries_from_matrix(DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_RGB, M,
                                                       &primaries))
    return FALSE;
  if(!dt_iop_channelmixer_shared_primaries_to_matrix(DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_RGB, &primaries,
                                                     roundtrip))
    return FALSE;

  const float roundtrip_error = dt_iop_channelmixer_shared_roundtrip_error(M, roundtrip);
  if(error) *error = roundtrip_error;

  ++darktable.gui->reset;
  dt_iop_channelmixer_shared_primaries_to_sliders(&primaries, widgets);
  --darktable.gui->reset;

  return isfinite(roundtrip_error) && roundtrip_error <= DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS;
}

static void _queue_preview_redraw(dt_iop_module_t *self)
{
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  if(g && g->preview) gtk_widget_queue_draw(g->preview);
}

/**
 * @brief Refresh cached GUI colors after the darkroom pipe has converged.
 *
 * Slider backgrounds and the preview gradient depend on the current work and
 * display profiles. Those profiles may be unavailable during the very first
 * draw after the module widget is created, so we listen to pipe completion and
 * rebuild the cached surfaces as soon as the pipe is ready.
 *
 * @param[in] instance Unused signal emitter.
 * @param[in] user_data Current module instance.
 */
static void _pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;

  if(g == NULL) return;

  for(int point = 0; point < DT_SPLITTONING_RGB_POINT_COUNT; point++) _update_point_slider_colors(self, point);

  if(g->preview_surface)
  {
    cairo_surface_destroy(g->preview_surface);
    g->preview_surface = NULL;
  }
  g->preview_width = 0;
  g->preview_height = 0;
  _queue_preview_redraw(self);
}

/**
 * @brief Repaint every slider background for one split-toning keyframe.
 *
 * The complete, simple and primaries controls all act on the same effective
 * mixer, just viewed through different parameterizations. We therefore refresh
 * every slider tint from the current widget state after each edit so the GUI
 * keeps reflecting the exact correction the user is shaping.
 *
 * @param[in] self Current module instance.
 * @param[in] point Shadow or highlight keyframe being edited.
 */
static void _update_point_slider_colors(dt_iop_module_t *self, const int point)
{
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  const dt_iop_order_iccprofile_info_t *work_profile = self->dev && self->dev->pipe
                                                           ? dt_ioppr_get_pipe_work_profile_info(self->dev->pipe)
                                                           : NULL;
  const dt_iop_order_iccprofile_info_t *display_profile = self->dev && self->dev->pipe
                                                              ? dt_ioppr_get_pipe_output_profile_info(self->dev->pipe)
                                                              : NULL;
  const float *const rows[3] = { p->red[point], p->green[point], p->blue[point] };
  const float basis[3][3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };
  dt_iop_channelmixer_shared_simple_params_t simple;
  dt_iop_channelmixer_shared_primaries_params_t primaries;
  GtkWidget *const simple_widgets[6]
      = { g->point[point].simple_theta, g->point[point].simple_psi, g->point[point].simple_stretch_1,
          g->point[point].simple_stretch_2, g->point[point].simple_coupling_1, g->point[point].simple_coupling_2 };
  GtkWidget *const primaries_widgets[9]
      = { g->point[point].primaries_achromatic_hue, g->point[point].primaries_achromatic_purity,
          g->point[point].primaries_red_hue, g->point[point].primaries_red_purity,
          g->point[point].primaries_green_hue, g->point[point].primaries_green_purity,
          g->point[point].primaries_blue_hue, g->point[point].primaries_blue_purity,
          g->point[point].primaries_gain };

  dt_iop_channelmixer_shared_paint_temperature_slider(g->point[point].temperature, 1667.f, 25000.f);

  for(int row = 0; row < 3; row++)
  {
    GtkWidget *const complete_widgets[3]
        = { g->point[point].complete[row][0], g->point[point].complete[row][1], g->point[point].complete[row][2] };
    dt_iop_channelmixer_shared_paint_row_sliders(DT_ADAPTATION_RGB, work_profile, display_profile, basis[row][0],
                                                 basis[row][1], basis[row][2], p->normalize[point][row], rows[row],
                                                 complete_widgets);
  }

  dt_iop_channelmixer_shared_simple_from_sliders(simple_widgets, &simple);
  dt_iop_channelmixer_shared_paint_simple_sliders(DT_ADAPTATION_RGB, work_profile, display_profile, &simple,
                                                  simple_widgets);

  dt_iop_channelmixer_shared_primaries_from_sliders(primaries_widgets, &primaries);
  dt_iop_channelmixer_shared_paint_primaries_sliders(DT_ADAPTATION_RGB, work_profile, display_profile,
                                                     DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_RGB, &primaries,
                                                     primaries_widgets);
}

static void _update_point_gui(dt_iop_module_t *self, const int point, GtkWidget *changed)
{
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;

  const dt_iop_splittoning_rgb_mixer_mode_t active_mode = dt_bauhaus_combobox_get(g->point[point].mixer_mode);
  gboolean complete_widget = FALSE;

  for(int row = 0; row < 3; row++)
  {
    complete_widget = complete_widget || changed == g->point[point].normalize[row];
    for(int col = 0; col < 3; col++) complete_widget = complete_widget || changed == g->point[point].complete[row][col];
  }

  if(!changed || complete_widget)
  {
    float simple_error = INFINITY;
    float primaries_error = INFINITY;
    const gboolean simple_ok = _sync_simple_from_params(self, point, &simple_error);
    const gboolean primaries_ok = _sync_primaries_from_params(self, point, &primaries_error);

    if(active_mode == DT_SPLITTONING_RGB_MIXER_SIMPLE && !simple_ok)
    {
      ++darktable.gui->reset;
      dt_bauhaus_combobox_set(g->point[point].mixer_mode, DT_SPLITTONING_RGB_MIXER_COMPLETE);
      --darktable.gui->reset;
      _set_point_mixer_mode(g, point, DT_SPLITTONING_RGB_MIXER_COMPLETE);
      dt_conf_set_int(_mode_conf[point], DT_SPLITTONING_RGB_MIXER_COMPLETE);
      dt_control_log(_("simple mixer mode requires normalized rows with non-zero sums."));
    }
    else if(active_mode == DT_SPLITTONING_RGB_MIXER_PRIMARIES && !primaries_ok)
    {
      ++darktable.gui->reset;
      dt_bauhaus_combobox_set(g->point[point].mixer_mode, DT_SPLITTONING_RGB_MIXER_COMPLETE);
      --darktable.gui->reset;
      _set_point_mixer_mode(g, point, DT_SPLITTONING_RGB_MIXER_COMPLETE);
      dt_conf_set_int(_mode_conf[point], DT_SPLITTONING_RGB_MIXER_COMPLETE);
      dt_control_log(_("primaries mixer mode requires a non-singular 3x3 matrix with non-zero affine sums."));
    }
  }

  ++darktable.gui->reset;
  _set_point_complete_widgets(g, p, point);
  --darktable.gui->reset;
  _queue_preview_redraw(self);
}

static void _commit_gui_change(dt_iop_module_t *self, GtkWidget *changed)
{
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  const int point = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(changed), "split-point"));

  _update_point_slider_colors(self, point);
  if(g->preview_surface)
  {
    cairo_surface_destroy(g->preview_surface);
    g->preview_surface = NULL;
  }
  g->preview_width = 0;
  g->preview_height = 0;
  _queue_preview_redraw(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

/**
 * @brief Render the cached split-toning preview as a simple keyed gradient.
 *
 * The drawing area is meant as a direct preview of the keyed blend, not as a
 * plotting canvas. We therefore sweep a neutral grey ramp once, cache the
 * resulting Cairo image surface and only rebuild it when parameters change or
 * the widget size changes.
 *
 * @param[in] self Current module instance.
 * @param[in,out] surface Cached Cairo image surface to repaint.
 */
static void _render_preview_surface(dt_iop_module_t *self, cairo_surface_t *surface)
{
  if(!self->dev || !self->dev->preview_pipe) return;

  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_work_profile_info(self->dev->preview_pipe);
  const dt_iop_order_iccprofile_info_t *display_profile = dt_ioppr_get_pipe_output_profile_info(self->dev->preview_pipe);
  if(!work_profile || !display_profile) return;

  dt_iop_splittoning_rgb_data_t state = { 0 };
  cairo_t *cr = cairo_create(surface);
  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);

  if(_build_point_transform(p, DT_SPLITTONING_RGB_POINT_DARK, work_profile,
                            state.point_matrix[DT_SPLITTONING_RGB_POINT_DARK]))
    for(int i = 0; i < 3; i++) state.point_matrix[DT_SPLITTONING_RGB_POINT_DARK][i][i] = 1.f;
  if(_build_point_transform(p, DT_SPLITTONING_RGB_POINT_BRIGHT, work_profile,
                            state.point_matrix[DT_SPLITTONING_RGB_POINT_BRIGHT]))
    for(int i = 0; i < 3; i++) state.point_matrix[DT_SPLITTONING_RGB_POINT_BRIGHT][i][i] = 1.f;

  memset(state.rgb_to_xyz_transposed, 0, sizeof(dt_colormatrix_t));
  if(work_profile)
    memcpy(state.rgb_to_xyz_transposed, work_profile->matrix_in_transposed, sizeof(dt_colormatrix_t));
  else
  {
    for(int i = 0; i < 3; i++) state.rgb_to_xyz_transposed[i][i] = 1.f;
  }

  state.dark_luminance = _ev_to_luminance(p->ev[DT_SPLITTONING_RGB_POINT_DARK]);
  state.bright_luminance = _ev_to_luminance(p->ev[DT_SPLITTONING_RGB_POINT_BRIGHT]);
  if(state.bright_luminance <= state.dark_luminance)
    state.bright_luminance = state.dark_luminance + fmaxf(state.dark_luminance * 0.01f, 1e-4f);
  for(int x = 0; x < width; x++)
  {
    const float grey = width > 1 ? (float)x / (float)(width - 1) : 0.f;
    dt_aligned_pixel_t input = { grey, grey, grey, 0.f };
    dt_aligned_pixel_t XYZ = { 0.f };
    dt_aligned_pixel_t output = { 0.f };
    dt_aligned_pixel_t display = { 0.f };
    dt_colormatrix_t combined_matrix = { { 0.f } };
    const dt_aligned_pixel_simd_t input_v = dt_load_simd_aligned(input);

    dt_apply_transposed_color_matrix(input, state.rgb_to_xyz_transposed, XYZ);
    _get_split_matrix(fmaxf(XYZ[1], 0.f), &state, combined_matrix);

    dt_store_simd_aligned(output, dt_mat3x4_mul_vec4(input_v,
                                                     dt_colormatrix_row_to_simd(combined_matrix, 0),
                                                     dt_colormatrix_row_to_simd(combined_matrix, 1),
                                                     dt_colormatrix_row_to_simd(combined_matrix, 2)));

    dt_iop_channelmixer_shared_work_rgb_to_display(output, work_profile, display_profile, display);
    cairo_set_source_rgb(cr, display[0], display[1], display[2]);
    cairo_rectangle(cr, x, 0, 1, height);
    cairo_fill(cr);
  }

  cairo_destroy(cr);
  cairo_surface_mark_dirty(surface);
}

static gboolean _preview_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  GtkAllocation allocation;

  gtk_widget_get_allocation(widget, &allocation);
  if(allocation.width <= 0 || allocation.height <= 0) return TRUE;

  if(!g->preview_surface || g->preview_width != allocation.width || g->preview_height != allocation.height)
  {
    if(g->preview_surface) cairo_surface_destroy(g->preview_surface);
    g->preview_surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, allocation.width, allocation.height);
    if(g->preview_surface == NULL) return FALSE;
    g->preview_width = allocation.width;
    g->preview_height = allocation.height;
    _render_preview_surface(self, g->preview_surface);
  }

  cairo_set_source_surface(cr, g->preview_surface, 0., 0.);
  cairo_paint(cr);

  return TRUE;
}

static void _general_callback(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  const int point = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "split-point"));
  gboolean changed_complete = FALSE;

  if(widget == g->point[point].ev)
    p->ev[point] = dt_bauhaus_slider_get(widget);
  else if(widget == g->point[point].temperature)
    p->temperature[point] = dt_bauhaus_slider_get(widget);

  for(int row = 0; row < 3; row++)
  {
    if(widget == g->point[point].normalize[row])
    {
      p->normalize[point][row] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
      changed_complete = TRUE;
    }

    for(int col = 0; col < 3; col++)
    {
      if(widget == g->point[point].complete[row][col])
      {
        float *target = row == 0 ? &p->red[point][col] : row == 1 ? &p->green[point][col] : &p->blue[point][col];
        *target = dt_bauhaus_slider_get(widget);
        changed_complete = TRUE;
      }
    }
  }

  if(changed_complete) _update_point_gui(self, point, widget);
  _commit_gui_change(self, widget);
}

static void _mixer_mode_callback(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  const int point = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "split-point"));
  const dt_iop_splittoning_rgb_mixer_mode_t mode = dt_bauhaus_combobox_get(widget);

  dt_conf_set_int(_mode_conf[point], mode);

  if(mode == DT_SPLITTONING_RGB_MIXER_SIMPLE)
  {
    float error = INFINITY;
    if(!_sync_simple_from_params(self, point, &error))
    {
      dt_control_log(_("simple mixer mode requires normalized rows with non-zero sums."));
      ++darktable.gui->reset;
      dt_bauhaus_combobox_set(widget, DT_SPLITTONING_RGB_MIXER_COMPLETE);
      --darktable.gui->reset;
      _set_point_mixer_mode(g, point, DT_SPLITTONING_RGB_MIXER_COMPLETE);
      dt_conf_set_int(_mode_conf[point], DT_SPLITTONING_RGB_MIXER_COMPLETE);
      return;
    }
  }
  else if(mode == DT_SPLITTONING_RGB_MIXER_PRIMARIES)
  {
    float error = INFINITY;
    if(!_sync_primaries_from_params(self, point, &error))
    {
      dt_control_log(_("primaries mixer mode requires a non-singular 3x3 matrix with non-zero affine sums."));
      ++darktable.gui->reset;
      dt_bauhaus_combobox_set(widget, DT_SPLITTONING_RGB_MIXER_COMPLETE);
      --darktable.gui->reset;
      _set_point_mixer_mode(g, point, DT_SPLITTONING_RGB_MIXER_COMPLETE);
      dt_conf_set_int(_mode_conf[point], DT_SPLITTONING_RGB_MIXER_COMPLETE);
      return;
    }
  }

  if(mode == DT_SPLITTONING_RGB_MIXER_SIMPLE)
    for(int row = 0; row < 3; row++) p->normalize[point][row] = TRUE;

  ++darktable.gui->reset;
  _set_point_complete_widgets(g, p, point);
  _set_point_mixer_mode(g, point, mode);
  --darktable.gui->reset;
  _commit_gui_change(self, widget);
}

static void _simple_slider_callback(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  const int point = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "split-point"));
  GtkWidget *const widgets[6]
      = { g->point[point].simple_theta, g->point[point].simple_psi, g->point[point].simple_stretch_1,
          g->point[point].simple_stretch_2, g->point[point].simple_coupling_1,
          g->point[point].simple_coupling_2 };
  dt_iop_channelmixer_shared_simple_params_t simple;
  float M[3][3] = { { 0.f } };

  dt_iop_channelmixer_shared_simple_from_sliders(widgets, &simple);
  dt_iop_channelmixer_shared_simple_to_matrix(&simple, M);
  _set_point_rows(p, point, M);

  for(int row = 0; row < 3; row++) p->normalize[point][row] = TRUE;

  ++darktable.gui->reset;
  _set_point_complete_widgets(g, p, point);
  _sync_primaries_from_params(self, point, NULL);
  --darktable.gui->reset;

  _commit_gui_change(self, widget);
}

static void _primaries_slider_callback(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  const int point = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "split-point"));
  GtkWidget *const widgets[9]
      = { g->point[point].primaries_achromatic_hue, g->point[point].primaries_achromatic_purity,
          g->point[point].primaries_red_hue, g->point[point].primaries_red_purity,
          g->point[point].primaries_green_hue, g->point[point].primaries_green_purity,
          g->point[point].primaries_blue_hue, g->point[point].primaries_blue_purity,
          g->point[point].primaries_gain };
  dt_iop_channelmixer_shared_primaries_params_t primaries;
  float M[3][3] = { { 0.f } };

  dt_iop_channelmixer_shared_primaries_from_sliders(widgets, &primaries);
  if(!dt_iop_channelmixer_shared_primaries_to_matrix(DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_RGB, &primaries, M))
  {
    dt_control_log(_("primaries mixer mode requires a non-singular 3x3 matrix with non-zero affine sums."));
    return;
  }

  _set_point_rows(p, point, M);
  for(int row = 0; row < 3; row++) p->normalize[point][row] = FALSE;

  ++darktable.gui->reset;
  _set_point_complete_widgets(g, p, point);
  --darktable.gui->reset;

  _commit_gui_change(self, widget);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)params;
  dt_iop_splittoning_rgb_data_t *d = (dt_iop_splittoning_rgb_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, pipe);
  dt_colormatrix_t point_matrix = { { 0.f } };
  dt_colormatrix_t rgb_to_xyz = { { 0.f } };

  if(_build_point_transform(p, DT_SPLITTONING_RGB_POINT_DARK, work_profile,
                            d->point_matrix[DT_SPLITTONING_RGB_POINT_DARK]))
    for(int i = 0; i < 3; i++) d->point_matrix[DT_SPLITTONING_RGB_POINT_DARK][i][i] = 1.f;
  if(_build_point_transform(p, DT_SPLITTONING_RGB_POINT_BRIGHT, work_profile,
                            d->point_matrix[DT_SPLITTONING_RGB_POINT_BRIGHT]))
    for(int i = 0; i < 3; i++) d->point_matrix[DT_SPLITTONING_RGB_POINT_BRIGHT][i][i] = 1.f;

  memset(d->rgb_to_xyz_transposed, 0, sizeof(dt_colormatrix_t));
  if(work_profile)
    memcpy(d->rgb_to_xyz_transposed, work_profile->matrix_in_transposed, sizeof(dt_colormatrix_t));
  else
  {
    for(int i = 0; i < 3; i++) d->rgb_to_xyz_transposed[i][i] = 1.f;
  }

  for(int point = 0; point < DT_SPLITTONING_RGB_POINT_COUNT; point++)
  {
    transpose_3xSSE(d->point_matrix[point], point_matrix);
    pack_3xSSE_to_3x4(point_matrix, d->point_matrix_cl[point]);
  }

  transpose_3xSSE(d->rgb_to_xyz_transposed, rgb_to_xyz);
  pack_3xSSE_to_3x4(rgb_to_xyz, d->rgb_to_xyz_cl);

  d->dark_luminance = _ev_to_luminance(p->ev[DT_SPLITTONING_RGB_POINT_DARK]);
  d->bright_luminance = _ev_to_luminance(p->ev[DT_SPLITTONING_RGB_POINT_BRIGHT]);
  if(d->bright_luminance <= d->dark_luminance)
    d->bright_luminance = d->dark_luminance + fmaxf(d->dark_luminance * 0.01f, 1e-4f);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_splittoning_rgb_data_t));
  piece->data_size = sizeof(dt_iop_splittoning_rgb_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const restrict ivoid, void *const restrict ovoid)
{
  const dt_iop_splittoning_rgb_data_t *d = (dt_iop_splittoning_rgb_data_t *)piece->data;
  const float *const restrict in = (const float *const restrict)ivoid;
  float *const restrict out = (float *const restrict)ovoid;
  const size_t width = piece->roi_out.width;
  const size_t height = piece->roi_out.height;
  const size_t pixels = width * height;

#ifdef _OPENMP
#pragma omp parallel for dt_omp_default() firstprivate(in, out, width, height, pixels, d) schedule(simd:static)
#endif
  for(size_t k = 0; k < pixels * 4; k += 4)
  {
    dt_aligned_pixel_t input = { in[k + 0], in[k + 1], in[k + 2], in[k + 3] };
    dt_aligned_pixel_t XYZ = { 0.f };
    dt_colormatrix_t combined_matrix = { { 0.f } };
    const dt_aligned_pixel_simd_t input_v = dt_load_simd_aligned(input);

    dt_store_simd_aligned(XYZ,
                          dt_mat3x4_mul_vec4(input_v, dt_colormatrix_row_to_simd(d->rgb_to_xyz_transposed, 0),
                                             dt_colormatrix_row_to_simd(d->rgb_to_xyz_transposed, 1),
                                             dt_colormatrix_row_to_simd(d->rgb_to_xyz_transposed, 2)));

    _get_split_matrix(fmaxf(XYZ[1], 0.f), d, combined_matrix);

    dt_store_simd_aligned(out + k, dt_mat3x4_mul_vec4(input_v,
                                                      dt_colormatrix_row_to_simd(combined_matrix, 0),
                                                      dt_colormatrix_row_to_simd(combined_matrix, 1),
                                                      dt_colormatrix_row_to_simd(combined_matrix, 2)));
    out[k + 3] = input[3];
  }

  dt_omploop_sfence();
  return 0;
}

#if HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_splittoning_rgb_data_t *d = (dt_iop_splittoning_rgb_data_t *)piece->data;
  dt_iop_splittoning_rgb_global_data_t *gd = (dt_iop_splittoning_rgb_global_data_t *)self->global_data;

  cl_int err = -999;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem rgb_to_xyz_cl = NULL;
  cl_mem dark_matrix_cl = NULL;
  cl_mem bright_matrix_cl = NULL;

  rgb_to_xyz_cl
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(d->rgb_to_xyz_cl), (void *)d->rgb_to_xyz_cl);
  dark_matrix_cl = dt_opencl_copy_host_to_device_constant(
      devid, sizeof(d->point_matrix_cl[DT_SPLITTONING_RGB_POINT_DARK]),
      (void *)d->point_matrix_cl[DT_SPLITTONING_RGB_POINT_DARK]);
  bright_matrix_cl = dt_opencl_copy_host_to_device_constant(
      devid, sizeof(d->point_matrix_cl[DT_SPLITTONING_RGB_POINT_BRIGHT]),
      (void *)d->point_matrix_cl[DT_SPLITTONING_RGB_POINT_BRIGHT]);

  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 4, sizeof(cl_mem), (void *)&rgb_to_xyz_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 5, sizeof(cl_mem), (void *)&dark_matrix_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 6, sizeof(cl_mem), (void *)&bright_matrix_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 7, sizeof(float), (void *)&d->dark_luminance);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoningrgb, 8, sizeof(float), (void *)&d->bright_luminance);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_splittoningrgb, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(rgb_to_xyz_cl);
  dt_opencl_release_mem_object(dark_matrix_cl);
  dt_opencl_release_mem_object(bright_matrix_cl);
  return TRUE;

error:
  dt_opencl_release_mem_object(rgb_to_xyz_cl);
  dt_opencl_release_mem_object(dark_matrix_cl);
  dt_opencl_release_mem_object(bright_matrix_cl);
  dt_print(DT_DEBUG_OPENCL, "[opencl_splittoningrgb] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 32; // channelmixer.cl, from programs.conf
  dt_iop_splittoning_rgb_global_data_t *gd
      = (dt_iop_splittoning_rgb_global_data_t *)malloc(sizeof(dt_iop_splittoning_rgb_global_data_t));

  module->data = gd;
  gd->kernel_splittoningrgb = dt_opencl_create_kernel(program, "splittoningrgb");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_splittoning_rgb_global_data_t *gd = (dt_iop_splittoning_rgb_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_splittoningrgb);
  dt_free(module->data);
}
#endif

/**
 * @brief Set a brightness keyframe from the average luminance of a picked area.
 *
 * The picker samples the current module buffer in working RGB. We convert that
 * average RGB to scene luminance Y in the active working profile, then map the
 * result to EV so each anchor can be snapped directly to image content.
 *
 * @param[in] self Current module instance.
 * @param[in] picker Picker-enabled brightness slider that requested the sample.
 * @param[in] pipe Active pixelpipe that produced the sampled buffer.
 * @param[in] piece Current module instance on that pipe.
 */
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(darktable.gui->reset) return;

  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;
  const dt_iop_module_t *const sampled_module = piece && piece->module ? piece->module : self;
  const dt_iop_order_iccprofile_info_t *const work_profile
      = pipe ? dt_ioppr_get_pipe_current_profile_info(self, pipe)
             : dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

  if(!g) return;
  if(sampled_module->picked_color_max[0] < sampled_module->picked_color_min[0]) return;

  const int point = picker == g->point[DT_SPLITTONING_RGB_POINT_DARK].ev
                        ? DT_SPLITTONING_RGB_POINT_DARK
                        : picker == g->point[DT_SPLITTONING_RGB_POINT_BRIGHT].ev
                              ? DT_SPLITTONING_RGB_POINT_BRIGHT
                              : -1;
  if(point < 0) return;

  const float luminance = work_profile
                              ? dt_ioppr_get_rgb_matrix_luminance(sampled_module->picked_color, work_profile->matrix_in,
                                                                  work_profile->lut_in,
                                                                  work_profile->unbounded_coeffs_in,
                                                                  work_profile->lutsize, work_profile->nonlinearlut)
                              : dt_camera_rgb_luminance(sampled_module->picked_color);

  p->ev[point] = CLAMP(log2f(fmaxf(luminance, NORM_MIN)), -16.f, 16.f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->point[point].ev, p->ev[point]);
  --darktable.gui->reset;

  _commit_gui_change(self, picker);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  dt_iop_splittoning_rgb_params_t *p = (dt_iop_splittoning_rgb_params_t *)self->params;

  ++darktable.gui->reset;

  for(int point = 0; point < DT_SPLITTONING_RGB_POINT_COUNT; point++)
  {
    dt_bauhaus_slider_set(g->point[point].ev, p->ev[point]);
    dt_bauhaus_slider_set(g->point[point].temperature, p->temperature[point]);
    _set_point_complete_widgets(g, p, point);

    float simple_error = INFINITY;
    float primaries_error = INFINITY;
    const gboolean simple_ok = _sync_simple_from_params(self, point, &simple_error);
    const gboolean primaries_ok = _sync_primaries_from_params(self, point, &primaries_error);
    const dt_iop_splittoning_rgb_mixer_mode_t requested_mode = dt_conf_key_exists(_mode_conf[point])
                                                                   ? dt_conf_get_int(_mode_conf[point])
                                                                   : DT_SPLITTONING_RGB_MIXER_SIMPLE;
    const dt_iop_splittoning_rgb_mixer_mode_t mode
        = requested_mode == DT_SPLITTONING_RGB_MIXER_SIMPLE && simple_ok
              ? DT_SPLITTONING_RGB_MIXER_SIMPLE
              : requested_mode == DT_SPLITTONING_RGB_MIXER_PRIMARIES && primaries_ok
                    ? DT_SPLITTONING_RGB_MIXER_PRIMARIES
                    : DT_SPLITTONING_RGB_MIXER_COMPLETE;

    dt_bauhaus_combobox_set(g->point[point].mixer_mode, mode);
    _set_point_mixer_mode(g, point, mode);
    _update_point_slider_colors(self, point);
  }

  --darktable.gui->reset;
  if(g->preview_surface)
  {
    cairo_surface_destroy(g->preview_surface);
    g->preview_surface = NULL;
  }
  g->preview_width = 0;
  g->preview_height = 0;
  _queue_preview_redraw(self);
}

static void _tag_widget(GtkWidget *widget, const int point)
{
  g_object_set_data(G_OBJECT(widget), "split-point", GINT_TO_POINTER(point));
}

static void _build_complete_ui(dt_iop_module_t *self, dt_iop_splittoning_rgb_gui_data_t *g, const int point,
                               GtkWidget *container)
{
  static const char *const row_label[3] = { N_("output red"), N_("output green"), N_("output blue") };
  static const char *const input_label[3] = { N_("input R"), N_("input G"), N_("input B") };

  for(int row = 0; row < 3; row++)
  {
    gtk_box_pack_start(GTK_BOX(container), dt_ui_section_label_new(_(row_label[row])), FALSE, FALSE, 0);

    // Each row scans the three input channels explicitly because the sliders own the output basis.
    for(int col = 0; col < 3; col++)
    {
      g->point[point].complete[row][col]
          = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -2.f, 2.f, 0, row == col, 3);
      dt_bauhaus_widget_set_label(g->point[point].complete[row][col], input_label[col]);
      _tag_widget(g->point[point].complete[row][col], point);
      g_signal_connect(G_OBJECT(g->point[point].complete[row][col]), "value-changed", G_CALLBACK(_general_callback),
                       self);
      gtk_box_pack_start(GTK_BOX(container), g->point[point].complete[row][col], FALSE, FALSE, 0);
    }

    g->point[point].normalize[row] = gtk_check_button_new_with_label(_("normalize"));
    _tag_widget(g->point[point].normalize[row], point);
    g_signal_connect(G_OBJECT(g->point[point].normalize[row]), "toggled", G_CALLBACK(_general_callback), self);
    gtk_box_pack_start(GTK_BOX(container), g->point[point].normalize[row], FALSE, FALSE, 0);
  }
}

static void _build_simple_ui(dt_iop_module_t *self, dt_iop_splittoning_rgb_gui_data_t *g, const int point,
                             GtkWidget *container)
{
  g->point[point].simple_theta
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.f, 1.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].simple_theta, N_("global hue rotation"));
  dt_bauhaus_slider_set_factor(g->point[point].simple_theta, 180.f);
  dt_bauhaus_slider_set_format(g->point[point].simple_theta, "\302\260");

  g->point[point].simple_psi
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.f, 1.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].simple_psi, N_("chroma (u,v) axes orientation"));
  dt_bauhaus_slider_set_factor(g->point[point].simple_psi, 90.f);
  dt_bauhaus_slider_set_format(g->point[point].simple_psi, "\302\260");

  g->point[point].simple_stretch_1
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.5f, 1.5f, 0, 1.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].simple_stretch_1, N_("u stretch"));

  g->point[point].simple_stretch_2
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.5f, 1.5f, 0, 1.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].simple_stretch_2, N_("v stretch"));

  g->point[point].simple_coupling_2
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.f, 1.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].simple_coupling_2, N_("achromatic coupling hue"));
  dt_bauhaus_slider_set_factor(g->point[point].simple_coupling_2, 180.f);
  dt_bauhaus_slider_set_format(g->point[point].simple_coupling_2, "\302\260");

  g->point[point].simple_coupling_1
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.f, 1.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].simple_coupling_1, N_("achromatic coupling amount"));

  GtkWidget *widgets[] = {
    g->point[point].simple_theta,
    dt_ui_section_label_new(_("chroma")),
    g->point[point].simple_psi,
    g->point[point].simple_stretch_1,
    g->point[point].simple_stretch_2,
    dt_ui_section_label_new(_("achromatic coupling")),
    g->point[point].simple_coupling_2,
    g->point[point].simple_coupling_1,
  };

  for(size_t i = 0; i < G_N_ELEMENTS(widgets); i++)
  {
    if(i != 1 && i != 5)
    {
      _tag_widget(widgets[i], point);
      g_signal_connect(G_OBJECT(widgets[i]), "value-changed", G_CALLBACK(_simple_slider_callback), self);
    }
    gtk_box_pack_start(GTK_BOX(container), widgets[i], FALSE, FALSE, 0);
  }
}

static void _build_primaries_ui(dt_iop_module_t *self, dt_iop_splittoning_rgb_gui_data_t *g, const int point,
                                GtkWidget *container)
{
  g->point[point].primaries_achromatic_hue
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.f, 1.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_achromatic_hue, N_("white hue"));
  dt_bauhaus_slider_set_factor(g->point[point].primaries_achromatic_hue, 90.f);
  dt_bauhaus_slider_set_format(g->point[point].primaries_achromatic_hue, "\302\260");

  g->point[point].primaries_achromatic_purity
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.f, 2.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_achromatic_purity, N_("white purity"));

  g->point[point].primaries_red_hue
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.f, 1.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_red_hue, N_("red hue"));
  dt_bauhaus_slider_set_factor(g->point[point].primaries_red_hue, 90.f);
  dt_bauhaus_slider_set_format(g->point[point].primaries_red_hue, "\302\260");

  g->point[point].primaries_red_purity
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.f, 2.f, 0, 1.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_red_purity, N_("red purity"));

  g->point[point].primaries_green_hue
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.f, 1.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_green_hue, N_("green hue"));
  dt_bauhaus_slider_set_factor(g->point[point].primaries_green_hue, 90.f);
  dt_bauhaus_slider_set_format(g->point[point].primaries_green_hue, "\302\260");

  g->point[point].primaries_green_purity
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.f, 2.f, 0, 1.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_green_purity, N_("green purity"));

  g->point[point].primaries_blue_hue
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -1.f, 1.f, 0, 0.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_blue_hue, N_("blue hue"));
  dt_bauhaus_slider_set_factor(g->point[point].primaries_blue_hue, 90.f);
  dt_bauhaus_slider_set_format(g->point[point].primaries_blue_hue, "\302\260");

  g->point[point].primaries_blue_purity
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.f, 2.f, 0, 1.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_blue_purity, N_("blue purity"));

  g->point[point].primaries_gain
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -8.f, 8.f, 0, 1.f, 3);
  dt_bauhaus_widget_set_label(g->point[point].primaries_gain, N_("gain"));

  GtkWidget *widgets[] = {
    dt_ui_section_label_new(_("achromatic axis")),
    g->point[point].primaries_achromatic_hue,
    g->point[point].primaries_achromatic_purity,
    dt_ui_section_label_new(_("red primary")),
    g->point[point].primaries_red_hue,
    g->point[point].primaries_red_purity,
    dt_ui_section_label_new(_("green primary")),
    g->point[point].primaries_green_hue,
    g->point[point].primaries_green_purity,
    dt_ui_section_label_new(_("blue primary")),
    g->point[point].primaries_blue_hue,
    g->point[point].primaries_blue_purity,
    dt_ui_section_label_new(_("gain correction")),
    g->point[point].primaries_gain,
  };

  for(size_t i = 0; i < G_N_ELEMENTS(widgets); i++)
  {
    if(i % 3 != 0)
    {
      _tag_widget(widgets[i], point);
      g_signal_connect(G_OBJECT(widgets[i]), "value-changed", G_CALLBACK(_primaries_slider_callback), self);
    }
    gtk_box_pack_start(GTK_BOX(container), widgets[i], FALSE, FALSE, 0);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_splittoning_rgb_gui_data_t *g = IOP_GUI_ALLOC(splittoning_rgb);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->preview_surface = NULL;
  g->preview_width = 0;
  g->preview_height = 0;

  g->preview = GTK_WIDGET(gtk_drawing_area_new());
  gtk_widget_set_size_request(g->preview, -1, DT_PIXEL_APPLY_DPI(DT_SPLITTONING_RGB_PREVIEW_HEIGHT));
  g_signal_connect(G_OBJECT(g->preview), "draw", G_CALLBACK(_preview_draw), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->preview, FALSE, FALSE, 0);

  g->tabs = dt_ui_notebook_new();
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->tabs), FALSE, FALSE, 0);

  for(int point = 0; point < DT_SPLITTONING_RGB_POINT_COUNT; point++)
  {
    GtkWidget *page = dt_ui_notebook_page(g->tabs, _point_label[point], _(point == 0 ? "shadows" : "highlights"));
    g->point[point].page = page;

    g->point[point].ev = dt_color_picker_new(
        self, DT_COLOR_PICKER_AREA,
        dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), -16.f, 16.f, 0,
                                         point == 0 ? -16.f : 0.f, 2));
    dt_bauhaus_widget_set_label(g->point[point].ev, N_("brightness"));
    dt_bauhaus_slider_set_format(g->point[point].ev, " EV");
    gtk_widget_set_tooltip_text(g->point[point].ev, _("sample average luminance from an area to set this keyframe"));
    _tag_widget(g->point[point].ev, point);
    g_signal_connect(G_OBJECT(g->point[point].ev), "value-changed", G_CALLBACK(_general_callback), self);
    gtk_box_pack_start(GTK_BOX(page), g->point[point].ev, FALSE, FALSE, 0);

    g->point[point].temperature
        = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 1667.f, 25000.f, 0, 5003.f, 0);
    dt_bauhaus_widget_set_label(g->point[point].temperature, N_("temperature"));
    dt_bauhaus_slider_set_soft_range(g->point[point].temperature, 3000.f, 7000.f);
    dt_bauhaus_slider_set_format(g->point[point].temperature, " K");
    _tag_widget(g->point[point].temperature, point);
    g_signal_connect(G_OBJECT(g->point[point].temperature), "value-changed", G_CALLBACK(_general_callback), self);
    gtk_box_pack_start(GTK_BOX(page), g->point[point].temperature, FALSE, FALSE, 0);

    g->point[point].mixer_mode = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
    dt_bauhaus_widget_set_label(g->point[point].mixer_mode, N_("mode"));
    dt_bauhaus_combobox_add(g->point[point].mixer_mode, _("Complete"));
    dt_bauhaus_combobox_add(g->point[point].mixer_mode, _("Simple"));
    dt_bauhaus_combobox_add(g->point[point].mixer_mode, _("Primaries"));
    _tag_widget(g->point[point].mixer_mode, point);
    g_signal_connect(G_OBJECT(g->point[point].mixer_mode), "value-changed", G_CALLBACK(_mixer_mode_callback), self);
    gtk_box_pack_start(GTK_BOX(page), g->point[point].mixer_mode, FALSE, FALSE, 0);

    g->point[point].mixer_stack = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(g->point[point].mixer_stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(g->point[point].mixer_stack), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(g->point[point].mixer_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_box_pack_start(GTK_BOX(page), g->point[point].mixer_stack, FALSE, FALSE, 0);

    GtkWidget *complete = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
    GtkWidget *simple = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
    GtkWidget *primaries = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
    gtk_stack_add_named(GTK_STACK(g->point[point].mixer_stack), complete, "complete");
    gtk_stack_add_named(GTK_STACK(g->point[point].mixer_stack), simple, "simple");
    gtk_stack_add_named(GTK_STACK(g->point[point].mixer_stack), primaries, "primaries");

    _build_complete_ui(self, g, point, complete);
    _build_simple_ui(self, g, point, simple);
    _build_primaries_ui(self, g, point, primaries);
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                                  G_CALLBACK(_pipe_finished_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                  G_CALLBACK(_pipe_finished_callback), self);

  gui_update(self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_splittoning_rgb_gui_data_t *g = (dt_iop_splittoning_rgb_gui_data_t *)self->gui_data;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_pipe_finished_callback), self);
  if(g->preview_surface) cairo_surface_destroy(g->preview_surface);
  IOP_GUI_FREE;
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_splittoning_rgb_params_t *d = (dt_iop_splittoning_rgb_params_t *)module->default_params;
  memset(d, 0, sizeof(*d));

  d->ev[DT_SPLITTONING_RGB_POINT_DARK] = -16.f;
  d->ev[DT_SPLITTONING_RGB_POINT_BRIGHT] = 0.f;
  d->temperature[DT_SPLITTONING_RGB_POINT_DARK] = 5003.f;
  d->temperature[DT_SPLITTONING_RGB_POINT_BRIGHT] = 5003.f;

  for(int point = 0; point < DT_SPLITTONING_RGB_POINT_COUNT; point++)
    for(int row = 0; row < 3; row++)
    {
      d->normalize[point][row] = TRUE;
      d->red[point][row] = row == 0 ? 1.f : 0.f;
      d->green[point][row] = row == 1 ? 1.f : 0.f;
      d->blue[point][row] = row == 2 ? 1.f : 0.f;
    }

  module->default_enabled = FALSE;
}
