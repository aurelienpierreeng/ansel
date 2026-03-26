#pragma once

#include "develop/imageop.h"
#include "iop/drawlayer/paint.h"

/** @file
 *  @brief Shared drawlayer runtime helpers used across module/runtime files.
 */

#define DRAWLAYER_NAME_SIZE 64
#define DRAWLAYER_PROFILE_SIZE 256

typedef struct dt_iop_drawlayer_params_t
{
  unsigned int stroke_commit_hash; // $DEFAULT: 0
  char layer_name[DRAWLAYER_NAME_SIZE];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
  int64_t sidecar_timestamp; // $DEFAULT: 0
  int layer_order;           // $DEFAULT: -1
} dt_iop_drawlayer_params_t;

typedef struct dt_iop_drawlayer_gui_data_t dt_iop_drawlayer_gui_data_t;

typedef enum dt_drawlayer_runtime_feedback_t
{
  DT_DRAWLAYER_RUNTIME_FEEDBACK_NONE = 0,
  DT_DRAWLAYER_RUNTIME_FEEDBACK_FOCUS_LOSS_WAIT,
  DT_DRAWLAYER_RUNTIME_FEEDBACK_SAVE_WAIT,
} dt_drawlayer_runtime_feedback_t;

gboolean dt_drawlayer_commit_dabs(dt_iop_module_t *self, gboolean record_history);
gboolean dt_drawlayer_flush_layer_cache(dt_iop_module_t *self);
gboolean dt_drawlayer_sync_widget_cache(dt_iop_module_t *self);
void dt_drawlayer_set_pipeline_realtime_mode(dt_iop_module_t *self, gboolean state);
gboolean dt_drawlayer_build_worker_input_dab(dt_iop_module_t *self, dt_drawlayer_paint_stroke_t *state,
                                             const dt_drawlayer_paint_raw_input_t *input,
                                             dt_drawlayer_brush_dab_t *dab);
gboolean dt_drawlayer_ensure_layer_cache(dt_iop_module_t *self);
void dt_drawlayer_release_all_base_patch_extra_refs(dt_iop_drawlayer_gui_data_t *g);
void dt_drawlayer_touch_stroke_commit_hash(dt_iop_drawlayer_params_t *params, int dab_count,
                                           gboolean have_last_dab, float last_dab_x, float last_dab_y,
                                           uint32_t publish_serial);
void dt_drawlayer_begin_gui_stroke_capture(dt_iop_module_t *self,
                                           const dt_drawlayer_paint_raw_input_t *first_input);
void dt_drawlayer_end_gui_stroke_capture(dt_iop_module_t *self);
void dt_drawlayer_show_runtime_feedback(const dt_iop_drawlayer_gui_data_t *g,
                                        dt_drawlayer_runtime_feedback_t feedback);
void dt_drawlayer_wait_for_rasterization_modal(const dt_iop_drawlayer_gui_data_t *g,
                                               const char *title,
                                               const char *message);
void dt_drawlayer_set_os_cursor_hidden(gboolean hidden);
void gui_update(dt_iop_module_t *self);
