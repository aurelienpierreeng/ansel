#pragma once

#include "iop/drawlayer/coordinates.h"
#include "common/dtpthread.h"
#include "iop/drawlayer/widgets.h"
#include "iop/drawlayer/worker.h"

#ifdef HAVE_OPENCL
#include "common/opencl.h"
#endif

#include <stdint.h>

/** @file
 *  @brief Private runtime state/helpers shared by drawlayer module entrypoints.
 */

typedef struct dt_drawlayer_session_state_t
{
  gboolean pointer_valid;
  float last_view_x;
  float last_view_y;
  float last_view_scale;
  dt_drawlayer_damaged_rect_t live_view_rect;
  float live_padding;
  dt_drawlayer_damaged_rect_t preview_rect;
  drawlayer_view_patch_t live_patch;
  int preview_bg_mode;
  char missing_layer_error[256];
  gboolean background_job_running;
} dt_drawlayer_session_state_t;

typedef struct dt_drawlayer_process_state_t
{
  /** `base_patch`: RAM copy, converted as 32 bits floats of the full-res TIFF
   * layer. It's our interface between user interaction and disk file.
   */
  dt_drawlayer_cache_patch_t base_patch;
  /* `process_patch`: mutable, display-sized process tile used internally
   * while painting. Backend stroke workers update this buffer in-place to
   * provide low-latency realtime preview and incremental edits. Access to
   * `process_patch`, `process_read_patch`, and their meta-flags is serialized
   * through the cache-patch lock helpers backed by each patch `cache_entry`.
   * Use `process_patch_valid` / `process_patch_dirty` to query its state. */
  dt_drawlayer_cache_patch_t process_patch;

  /* `process_read_patch`: read-only host-side snapshot published for
   * consumers (GUI, pipeline, and safe GPU reads). This snapshot is created
   * by copying the authoritative `process_patch` during a publish step so
   * readers can access stable pixels without racing concurrent in-flight
   * edits. The snapshot may be backed by host-pinned memory for OpenCL and
   * is managed via the cache patch lock helpers (rdlock/wrunlock). */
  dt_drawlayer_cache_patch_t process_read_patch;

  /* `stroke_mask`: authoritative full-resolution stroke alpha mask aligned
   * with `base_patch`. This stores accumulated stroke coverage in base/source
   * coordinates and is used to pre-render brushes using flow to cap opacity. */
  dt_drawlayer_cache_patch_t stroke_mask;

  /* `process_stroke_mask`: display-sized mask derived from `stroke_mask` and
   * resampled to `process_patch` coordinates. This transformed mask is used
   * during realtime blending/preview and when folding incremental edits from
   * `process_patch` back into `base_patch` (the flush path). */
  dt_drawlayer_cache_patch_t process_stroke_mask;
  gboolean cache_valid;
  gboolean cache_dirty;

  int32_t cache_imgid;
  char cache_layer_name[DRAWLAYER_NAME_SIZE];
  int cache_layer_order;
  gboolean base_patch_loaded_ref;
  uint32_t base_patch_stroke_refs;
  /* true when `process_patch` contains valid pixel data for the current view */
  gboolean process_patch_valid;
  /* true when `process_patch` contains incremental edits that haven't been
   * folded back into `base_patch` (i.e. needs flush). */
  gboolean process_patch_dirty;
  /* true when `process_read_patch` contains a coherent published snapshot of
   * `process_patch` for GUI/process consumption. */
  gboolean process_snapshot_valid;
  dt_drawlayer_damaged_rect_t process_dirty_rect;
  int process_patch_padding;
#ifdef HAVE_OPENCL
  cl_mem process_read_clmem;
  int process_read_clmem_width;
  int process_read_clmem_height;
  int process_read_clmem_devid;
  gboolean process_read_clmem_dirty;
  dt_drawlayer_damaged_rect_t process_read_clmem_dirty_rect;
#endif
  dt_iop_roi_t process_combined_roi;
} dt_drawlayer_process_state_t;

typedef struct dt_drawlayer_stroke_state_t
{
  dt_drawlayer_worker_t *worker;
  guint stroke_sample_count;
  uint32_t stroke_event_index;
  gboolean last_dab_valid;
  float last_dab_x;
  float last_dab_y;
  gboolean finish_commit_pending;
  uint32_t current_stroke_batch;
} dt_drawlayer_stroke_state_t;

typedef struct dt_drawlayer_ui_state_t
{
  dt_drawlayer_widgets_t *widgets;
  cairo_surface_t *cursor_surface;
  int cursor_surface_size;
  double cursor_surface_ppd;
  float cursor_radius;
  float cursor_opacity;
  float cursor_hardness;
  int cursor_shape;
  float cursor_color[3];
} dt_drawlayer_ui_state_t;

typedef struct dt_drawlayer_controls_t
{
  GtkWidget *notebook;
  GtkWidget *brush_tab;
  GtkWidget *layer_tab;
  GtkWidget *input_tab;
  GtkWidget *preview_title;
  GtkWidget *preview_box;
  GtkWidget *layer_action_row;
  GtkWidget *layer_fill_title;
  GtkWidget *layer_fill_row;
  GtkWidget *brush_shape;
  GtkWidget *brush_mode;
  GtkWidget *color;
  GtkWidget *color_row;
  GtkWidget *color_swatch;
  GtkWidget *image_colorpicker;
  GtkWidget *image_colorpicker_source;
  GtkWidget *size;
  GtkWidget *distance;
  GtkWidget *smoothing;
  GtkWidget *opacity;
  GtkWidget *flow;
  GtkWidget *sprinkles;
  GtkWidget *sprinkle_size;
  GtkWidget *sprinkle_coarseness;
  GtkWidget *softness;
  GtkWidget *hdr_exposure;
  GtkWidget *layer_status;
  GtkWidget *layer_select;
  GtkWidget *preview_bg_image;
  GtkWidget *preview_bg_white;
  GtkWidget *preview_bg_grey;
  GtkWidget *preview_bg_black;
  GtkWidget *delete_layer;
  GtkWidget *create_layer;
  GtkWidget *rename_layer;
  GtkWidget *attach_layer;
  GtkWidget *create_background;
  GtkWidget *save_layer;
  GtkWidget *fill_white;
  GtkWidget *fill_black;
  GtkWidget *fill_transparent;
  GtkWidget *map_pressure_size;
  GtkWidget *map_pressure_opacity;
  GtkWidget *map_pressure_flow;
  GtkWidget *map_pressure_softness;
  GtkWidget *map_tilt_size;
  GtkWidget *map_tilt_opacity;
  GtkWidget *map_tilt_flow;
  GtkWidget *map_tilt_softness;
  GtkWidget *map_accel_size;
  GtkWidget *map_accel_opacity;
  GtkWidget *map_accel_flow;
  GtkWidget *map_accel_softness;
  GtkWidget *pressure_profile;
  GtkWidget *tilt_profile;
  GtkWidget *accel_profile;
} dt_drawlayer_controls_t;

typedef enum dt_drawlayer_runtime_actor_t
{
  DT_DRAWLAYER_RUNTIME_ACTOR_NONE = 0,
  DT_DRAWLAYER_RUNTIME_ACTOR_GUI,
  DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CPU,
  DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CL,
  DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_BACKEND,
  DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_FULLRES,
  DT_DRAWLAYER_RUNTIME_ACTOR_TIFF_IO,
  DT_DRAWLAYER_RUNTIME_ACTOR_COUNT,
} dt_drawlayer_runtime_actor_t;

typedef enum dt_drawlayer_runtime_buffer_t
{
  DT_DRAWLAYER_RUNTIME_BUFFER_BASE_PATCH = 0,
  DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_PATCH,
  DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_SNAPSHOT,
  DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_CL,
  DT_DRAWLAYER_RUNTIME_BUFFER_STROKE_MASK,
  DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_STROKE_MASK,
  DT_DRAWLAYER_RUNTIME_BUFFER_COUNT,
} dt_drawlayer_runtime_buffer_t;

typedef enum dt_drawlayer_runtime_event_t
{
  DT_DRAWLAYER_RUNTIME_EVENT_NONE = 0,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_ENTER,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_SCROLL,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_PIPE_FINISHED,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT,
  DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT,
  DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_BEFORE,
  DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_AFTER,
  DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_BEFORE,
  DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_AFTER,
  DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_BEGIN,
  DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_END,
  DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_BEGIN,
  DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_END,
  DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN,
  DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_END,
} dt_drawlayer_runtime_event_t;

typedef enum dt_drawlayer_runtime_raw_input_kind_t
{
  DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE = 0,
  DT_DRAWLAYER_RUNTIME_RAW_INPUT_SAMPLE,
  DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN,
  DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_END,
} dt_drawlayer_runtime_raw_input_kind_t;

typedef enum dt_drawlayer_runtime_commit_mode_t
{
  DT_DRAWLAYER_RUNTIME_COMMIT_NONE = 0,
  DT_DRAWLAYER_RUNTIME_COMMIT_QUIET,
  DT_DRAWLAYER_RUNTIME_COMMIT_HISTORY,
} dt_drawlayer_runtime_commit_mode_t;

typedef struct dt_drawlayer_runtime_result_t
{
  gboolean ok;
  gboolean raw_input_ok;
} dt_drawlayer_runtime_result_t;

typedef struct dt_drawlayer_runtime_private_t dt_drawlayer_runtime_private_t;

typedef struct dt_drawlayer_runtime_manager_t
{
  gboolean realtime_active;
  gboolean painting_active;
  gboolean background_job_running;
  dt_drawlayer_runtime_private_t *priv;
} dt_drawlayer_runtime_manager_t;

typedef struct dt_drawlayer_runtime_inputs_t
{
  const dt_drawlayer_session_state_t *session;
  const dt_drawlayer_process_state_t *process;
  const dt_drawlayer_stroke_state_t *stroke;
  const dt_drawlayer_worker_snapshot_t *worker;
  const dt_drawlayer_cache_patch_t *base_patch;
  gboolean base_patch_valid;
  gboolean base_patch_dirty;
  gboolean painting_active;
  gboolean gui_attached;
  gboolean module_focused;
  gboolean display_pipe;
  gboolean have_layer_selection;
  const char *selected_layer_name;
  int selected_layer_order;
  gboolean have_valid_output_roi;
  gboolean use_opencl;
  gboolean view_changed;
  gboolean padding_changed;
} dt_drawlayer_runtime_inputs_t;

typedef struct dt_drawlayer_runtime_action_request_t dt_drawlayer_runtime_action_request_t;

typedef struct dt_drawlayer_runtime_host_t
{
  void *user_data;
  void (*collect_inputs)(void *user_data,
                         dt_drawlayer_runtime_inputs_t *inputs,
                         dt_drawlayer_worker_snapshot_t *worker_snapshot);
  gboolean (*perform_action)(void *user_data,
                             const dt_drawlayer_runtime_action_request_t *action,
                             dt_drawlayer_runtime_result_t *result);
} dt_drawlayer_runtime_host_t;

typedef struct dt_iop_drawlayer_gui_data_t
{
  dt_drawlayer_session_state_t session;
  dt_drawlayer_process_state_t process;
  dt_drawlayer_stroke_state_t stroke;
  dt_drawlayer_runtime_manager_t manager;
  dt_drawlayer_ui_state_t ui;
  dt_drawlayer_controls_t controls;
} dt_iop_drawlayer_gui_data_t;

typedef struct dt_drawlayer_runtime_request_t
{
  dt_iop_module_t *self;
  const dt_dev_pixelpipe_t *pipe;
  dt_dev_pixelpipe_iop_t *piece;
  const dt_iop_drawlayer_params_t *runtime_params;
  dt_iop_drawlayer_gui_data_t *gui;
  dt_drawlayer_runtime_manager_t *manager;
  dt_drawlayer_process_state_t *process_state;
  gboolean display_pipe;
  const dt_iop_roi_t *roi_in;
  const dt_iop_roi_t *roi_out;
  gboolean use_opencl;
} dt_drawlayer_runtime_request_t;

typedef struct dt_drawlayer_runtime_context_t
{
  dt_drawlayer_runtime_request_t runtime;
  const dt_drawlayer_paint_raw_input_t *raw_input;
} dt_drawlayer_runtime_context_t;

typedef struct dt_drawlayer_process_view_t
{
  const dt_drawlayer_cache_patch_t *patch;
  dt_iop_roi_t blend_target_roi;
  dt_iop_roi_t source_process_roi;
  gboolean direct_copy;
} dt_drawlayer_process_view_t;

typedef enum dt_drawlayer_runtime_source_kind_t
{
  DT_DRAWLAYER_SOURCE_NONE = 0,
  DT_DRAWLAYER_SOURCE_GUI_PROCESS,
  DT_DRAWLAYER_SOURCE_HEADLESS_BASE,
} dt_drawlayer_runtime_source_kind_t;

typedef struct dt_drawlayer_runtime_source_t
{
  dt_drawlayer_runtime_source_kind_t kind;
  dt_drawlayer_process_view_t process_view;
  const float *pixels;
  dt_pixel_cache_entry_t *cache_entry;
  int width;
  int height;
  gboolean direct_copy;
  dt_iop_roi_t target_roi;
  dt_iop_roi_t source_roi;
  dt_drawlayer_runtime_buffer_t tracked_buffer;
  dt_drawlayer_runtime_actor_t tracked_actor;
  gboolean tracked_read_lock;
} dt_drawlayer_runtime_source_t;

typedef struct dt_drawlayer_runtime_release_t
{
  dt_drawlayer_process_state_t *process;
  dt_drawlayer_runtime_source_t *source;
} dt_drawlayer_runtime_release_t;

typedef struct dt_drawlayer_runtime_update_request_t
{
  dt_drawlayer_runtime_event_t event;
  dt_drawlayer_runtime_raw_input_kind_t raw_input_kind;
  const dt_drawlayer_runtime_inputs_t *inputs;
  gboolean flush_pending;
  dt_drawlayer_runtime_release_t release;
} dt_drawlayer_runtime_update_request_t;

void dt_drawlayer_process_state_init(dt_drawlayer_process_state_t *state);
void dt_drawlayer_process_state_cleanup(dt_drawlayer_process_state_t *state);
void dt_drawlayer_process_state_reset_stroke(dt_drawlayer_process_state_t *state);
void dt_drawlayer_process_state_invalidate(dt_drawlayer_process_state_t *state);
gboolean dt_drawlayer_process_state_publish_locked(dt_drawlayer_process_state_t *state,
                                                   const dt_drawlayer_damaged_rect_t *damage,
                                                   gboolean full_copy);
void dt_drawlayer_ui_cursor_clear(dt_drawlayer_ui_state_t *state);
void dt_drawlayer_runtime_manager_init(dt_drawlayer_runtime_manager_t *state);
void dt_drawlayer_runtime_manager_cleanup(dt_drawlayer_runtime_manager_t *state);
void dt_drawlayer_runtime_manager_note_buffer_lock(dt_drawlayer_runtime_manager_t *state,
                                                   dt_drawlayer_runtime_buffer_t buffer,
                                                   dt_drawlayer_runtime_actor_t actor,
                                                   gboolean write_lock,
                                                   gboolean acquire);
void dt_drawlayer_runtime_manager_note_sidecar_io(dt_drawlayer_runtime_manager_t *state, gboolean active);
void dt_drawlayer_runtime_manager_note_thread(dt_drawlayer_runtime_manager_t *state,
                                              dt_drawlayer_runtime_actor_t actor,
                                              gboolean active,
                                              gboolean waiting,
                                              guint queued);
dt_drawlayer_runtime_result_t dt_drawlayer_runtime_manager_update(dt_drawlayer_runtime_manager_t *state,
                                                                  const dt_drawlayer_runtime_update_request_t *request,
                                                                  const dt_drawlayer_runtime_host_t *host);
void dt_drawlayer_runtime_manager_bind_piece(dt_drawlayer_runtime_manager_t *headless_manager,
                                             dt_drawlayer_process_state_t *headless_process,
                                             dt_drawlayer_runtime_manager_t *gui_manager,
                                             dt_drawlayer_process_state_t *gui_process,
                                             gboolean display_pipe,
                                             dt_drawlayer_runtime_manager_t **runtime_manager,
                                             dt_drawlayer_process_state_t **runtime_process,
                                             gboolean *runtime_display_pipe);

#ifdef HAVE_OPENCL
void dt_drawlayer_process_state_clear_clmem(dt_drawlayer_process_state_t *state);
cl_mem dt_drawlayer_process_state_ensure_read_clmem_locked(dt_drawlayer_process_state_t *state, int devid);
#endif
