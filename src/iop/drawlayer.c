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

#include "common/image.h"
#include "common/imagebuf.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "common/dtpthread.h"
#include "bauhaus/bauhaus.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/dev_history.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe_cache.h"
#include "develop/noise_generator.h"
#include "gui/gtk.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"

#include <glib/gstdio.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_drawlayer_params_t)

/*
 * drawlayer architecture summary
 * ------------------------------
 *
 * This module stores a painted premultiplied RGBA layer in a half-float TIFF sidecar.
 * The persistent layer lives in full image coordinates (raw-sized canvas, but in the
 * module's current pipeline geometry, not in raw sensor geometry). The GUI keeps:
 *
 * 1. a full-resolution half-float cache of the selected TIFF layer (`base_patch`),
 * 2. a widget-sized ARGB preview overlay (`live_surface`) used only for immediate feedback,
 * 3. a worker thread that consumes stroke samples while the module has focus.
 *
 * The current stroke model is intentionally conservative:
 * - `distance` defines a spatial metronome for resampling the path independently of
 *   `mouse_moved` dispatch cadence.
 * - smoothing is applied only to the incoming raw cursor point before that resampling.
 * - the kept implementation uses a simple linear extrapolation from two anchor samples
 *   separated by one brush radius. Several "cleverer" variants were tried here
 *   (kernel caching, fixed-N smoothing windows, higher-order extrapolation, inertial
 *   direction filters), but they either benchmarked slower, introduced sampling
 *   dependence again, or created visible cusps/overshoot. Those attempts were removed
 *   and are documented locally near the code they affected.
 *
 * The guiding rule throughout this file is: keep the authoritative geometry in layer
 * space, and derive widget-space feedback from it, so GUI preview and pipeline output
 * cannot drift apart due to duplicated math.
 */

#define DRAWLAYER_NAME_SIZE 64
#define DRAWLAYER_PROFILE_SIZE 256
#define DRAWLAYER_WORKER_RING_CAPACITY 65536
#define DRAWLAYER_COMPARE_ANALYTIC_TIMINGS 1
#define DRAWLAYER_CONF_BASE "plugins/drawlayer/"
#define DRAWLAYER_CONF_BRUSH_SHAPE DRAWLAYER_CONF_BASE "brush_shape"
#define DRAWLAYER_CONF_BRUSH_MODE DRAWLAYER_CONF_BASE "brush_mode"
#define DRAWLAYER_CONF_COLOR_R DRAWLAYER_CONF_BASE "color_r"
#define DRAWLAYER_CONF_COLOR_G DRAWLAYER_CONF_BASE "color_g"
#define DRAWLAYER_CONF_COLOR_B DRAWLAYER_CONF_BASE "color_b"
#define DRAWLAYER_CONF_SOFTNESS DRAWLAYER_CONF_BASE "softness"
#define DRAWLAYER_CONF_OPACITY DRAWLAYER_CONF_BASE "opacity"
#define DRAWLAYER_CONF_FLOW DRAWLAYER_CONF_BASE "flow"
#define DRAWLAYER_CONF_BRISTLES DRAWLAYER_CONF_BASE "bristles"
#define DRAWLAYER_CONF_SPRINKLES DRAWLAYER_CONF_BASE "sprinkles"
#define DRAWLAYER_CONF_BRISTLE_SIZE DRAWLAYER_CONF_BASE "bristle_size"
#define DRAWLAYER_CONF_SPRINKLE_SIZE DRAWLAYER_CONF_BASE "sprinkle_size"
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

typedef enum drawlayer_mapping_profile_t
{
  DRAWLAYER_PROFILE_LINEAR = 0,
  DRAWLAYER_PROFILE_QUADRATIC = 1,
  DRAWLAYER_PROFILE_SQRT = 2,
} drawlayer_mapping_profile_t;

typedef enum dt_iop_drawlayer_brush_shape_t
{
  DT_IOP_DRAWLAYER_BRUSH_LINEAR = 0,     // $DESCRIPTION: "linear"
  DT_IOP_DRAWLAYER_BRUSH_GAUSSIAN = 1,   // $DESCRIPTION: "gaussian"
  DT_IOP_DRAWLAYER_BRUSH_QUADRATIC = 2,  // $DESCRIPTION: "quadratic"
  DT_IOP_DRAWLAYER_BRUSH_SIGMOIDAL = 3   // $DESCRIPTION: "sigmoidal"
} dt_iop_drawlayer_brush_shape_t;

typedef enum dt_iop_drawlayer_brush_mode_t
{
  DT_IOP_DRAWLAYER_MODE_PAINT = 0, // $DESCRIPTION: "paint"
  DT_IOP_DRAWLAYER_MODE_ERASE = 1, // $DESCRIPTION: "erase"
  DT_IOP_DRAWLAYER_MODE_BLUR = 2,  // $DESCRIPTION: "blur"
  DT_IOP_DRAWLAYER_MODE_SMUDGE = 3 // $DESCRIPTION: "smudge"
} dt_iop_drawlayer_brush_mode_t;

typedef struct dt_iop_drawlayer_params_t
{
  unsigned int stroke_commit_hash; // $DEFAULT: 0
  char layer_name[DRAWLAYER_NAME_SIZE];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
  int64_t sidecar_timestamp; // $DEFAULT: 0
  int layer_order; // $DEFAULT: -1
} dt_iop_drawlayer_params_t;

typedef struct drawlayer_dab_t
{
  /* Authoritative sample position in layer space (the same space as the cached TIFF layer). */
  float x;
  float y;
  /* Widget-space projection of the same sample, used only for immediate preview feedback. */
  float wx;
  float wy;
  /* Radius is expressed in layer-space pixels. */
  float radius;
  /* Local stroke direction in layer space, used to orient directional brush
   * microstructure such as bristles along the motion instead of keeping it
   * axis-aligned in image space. */
  float dir_x;
  float dir_y;
  /* Normalized to 0..1 before the hot brush loops. */
  float opacity;
  float flow;
  /* Extra alpha-noise controls. `bristles` is per-stroke stable, `sprinkles`
   * is regenerated per dab. Both are applied as multiplicative alpha maps in
   * brush-local coordinates. */
  float bristles;
  float sprinkles;
  float bristle_size;
  float sprinkle_size;
  /* Historical field name kept for ABI continuity: stores user-facing hardness. */
  float softness;
  /* `color` is already converted to working RGB; `display_color` remains in display RGB. */
  float color[3];
  float display_color[3];
  int shape;
  int mode;
  uint32_t stroke_batch;
  uint8_t stroke_pos;
} drawlayer_dab_t;

typedef struct drawlayer_raw_input_t
{
  /* Raw pointer event in widget space. Workers transform this into their own processing space. */
  float wx;
  float wy;
  float pressure;
  float tilt;
  float acceleration;
  gint64 event_ts;
  uint32_t stroke_batch;
  uint8_t stroke_pos;
} drawlayer_raw_input_t;

typedef enum drawlayer_stroke_pos_t
{
  DRAWLAYER_STROKE_FIRST = 0,
  DRAWLAYER_STROKE_MIDDLE = 1,
  DRAWLAYER_STROKE_END = 2,
} drawlayer_stroke_pos_t;

typedef enum drawlayer_rt_event_type_t
{
  DRAWLAYER_RT_EVENT_SAMPLE = 0,
  DRAWLAYER_RT_EVENT_STROKE_END = 1,
} drawlayer_rt_event_type_t;

typedef struct drawlayer_rt_event_t
{
  drawlayer_rt_event_type_t type;
  drawlayer_raw_input_t input;
} drawlayer_rt_event_t;

typedef struct drawlayer_rt_path_state_t
{
  /* Per-worker stroke expansion state. Each worker owns its own geometry history so
   * coordinate transforms, smoothing and distance sampling stay local to that worker. */
  GArray *history;
  drawlayer_dab_t last_input_dab;
  gboolean have_last_input_dab;
  float last_layer_x;
  float last_layer_y;
  gboolean have_last_point;
  gint64 last_event_ts;
  float distance_carry;
  uint32_t stroke_batch;
  /* Stroke-local "bristle" payload for smudge mode. This is a local brush-space
   * RGBA map (one carried sample per brush pixel in the current dab footprint),
   * updated as a moving average against what the brush encounters. */
  float *smudge_pixels;
  int smudge_width;
  int smudge_height;
  /* Per-stroke cached alpha-noise map for the "bristles" property. It is
   * generated once when a new stroke starts, then stamped identically at every
   * dab of that stroke. */
  float *bristle_map;
  int bristle_map_size;
  uint64_t bristle_map_seed;
  uint64_t bristle_map_cached_seed;
  float smudge_pickup_x;
  float smudge_pickup_y;
  gboolean have_smudge_pickup;
} drawlayer_rt_path_state_t;

typedef struct drawlayer_rt_state_t drawlayer_rt_state_t;

typedef enum drawlayer_preview_bg_mode_t
{
  DRAWLAYER_PREVIEW_BG_IMAGE = 0,
  DRAWLAYER_PREVIEW_BG_WHITE = 1,
  DRAWLAYER_PREVIEW_BG_GREY = 2,
  DRAWLAYER_PREVIEW_BG_BLACK = 3,
} drawlayer_preview_bg_mode_t;

typedef enum drawlayer_color_drag_mode_t
{
  DRAWLAYER_COLOR_DRAG_NONE = 0,
  DRAWLAYER_COLOR_DRAG_DISC = 1,
  DRAWLAYER_COLOR_DRAG_PLANE = 2,
} drawlayer_color_drag_mode_t;

typedef enum drawlayer_pick_source_t
{
  DRAWLAYER_PICK_SOURCE_INPUT = 0,
  DRAWLAYER_PICK_SOURCE_OUTPUT = 1,
} drawlayer_pick_source_t;

#define DRAWLAYER_COLOR_PICKER_SIZE 340
#define DRAWLAYER_COLOR_PICKER_HEIGHT 184
#define DRAWLAYER_COLOR_HISTORY_COUNT 10
#define DRAWLAYER_COLOR_HISTORY_COLS 5
#define DRAWLAYER_COLOR_HISTORY_ROWS 2
#define DRAWLAYER_COLOR_HISTORY_HEIGHT 44
#define DRAWLAYER_COLOR_PICKER_U_MAX 0.70710678f
#define DRAWLAYER_COLOR_PICKER_V_MAX 0.81649658f
#define DRAWLAYER_COLOR_PICKER_C_MAX 0.81649658f

typedef struct drawlayer_patch_t
{
  /* Integer-aligned rectangle in layer space storing RGBA float32 pixels in memory.
   * TIFF I/O still uses half-float rows at the file boundary only. */
  int x;
  int y;
  int width;
  int height;
  float *pixels;
  /* Persistent patches can be backed by a real pixelpipe cache line, while short-lived
   * scratch patches can use externally tracked cache allocations. The metadata here keeps
   * ownership explicit so releasing a patch also releases the cache accounting. */
  dt_pixel_cache_entry_t *cache_entry;
  uint64_t cache_hash;
  gboolean external_alloc;
} drawlayer_patch_t;

typedef struct drawlayer_dir_info_t
{
  gboolean found;
  int index;
  int count;
  uint32_t width;
  uint32_t height;
  char name[DRAWLAYER_NAME_SIZE];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
} drawlayer_dir_info_t;

typedef struct drawlayer_stroke_t
{
  gboolean active;
  gboolean layer_damage_valid;
  int layer_x0;
  int layer_y0;
  int layer_x1;
  int layer_y1;
  gboolean widget_damage_valid;
  int widget_x0;
  int widget_y0;
  int widget_x1;
  int widget_y1;
} drawlayer_stroke_t;

typedef struct dt_iop_drawlayer_gui_data_t
{
  /* Reentrancy guard for GUI updates while widgets are being synchronized from state. */
  gboolean updating;
  /* True between button-press and button-release while a stroke is being captured. */
  gboolean painting;
  /* Cached pointer position in widget coordinates for cursor preview drawing. */
  gboolean pointer_valid;
  float pointer_x;
  float pointer_y;
  /* Last darkroom ROI state seen by the GUI; used to detect pan/zoom changes. */
  float last_view_x;
  float last_view_y;
  float last_view_scale;
  /* Visible layer-space rectangle currently mirrored by the transient GUI overlay. */
  float live_view_x0;
  float live_view_y0;
  float live_view_x1;
  float live_view_y1;
  /* Extra layer-space padding around the visible view so strokes can extend off-screen. */
  float live_padding;
  /* Widget-space rectangle where the transient overlay cairo surface is painted. */
  float preview_x0;
  float preview_y0;
  float preview_x1;
  float preview_y1;

  /* `preview_dabs` and `backend_dabs` are the short rolling windows used by the two workers
   * to interpolate the next local segment. `preview_history` and `backend_history` keep the
   * recent uniformly spaced stroke stream used for worker-local smoothing. */
  /* Full uniformly resampled stroke history for the current stroke, kept for fallback logic
   * and for operations that still need the whole stroke stream. */
  GArray *dabs;
  /* Rolling interpolation window for the preview worker only. */
  GArray *preview_dabs;
  /* Rolling interpolation window for the backend worker only. */
  GArray *backend_dabs;
  /* Uniformly spaced recent path history used by the preview worker smoothing logic. */
  GArray *preview_history;
  /* Uniformly spaced recent path history used by the backend worker smoothing logic. */
  GArray *backend_history;
  /* Per-worker raw-input to resampled-path conversion state (distance carry, last raw point, etc.). */
  drawlayer_rt_path_state_t preview_path;
  drawlayer_rt_path_state_t backend_path;
  /* Separate path state for the transformed process tile. It mirrors the
   * backend stroke stream, but lives in the current process-tile coordinate
   * system so smudge, flow=0 masks and other stroke-stateful modes can be
   * updated incrementally without re-running a full-tile resample. */
  drawlayer_rt_path_state_t process_path;
  /* `base_patch` mirrors the selected TIFF page in memory at full resolution. */
  drawlayer_patch_t base_patch;
  /* `process_patch` is the current visible tile in process/output coordinates
   * (module ROI size). Backend strokes update it directly for realtime feedback,
   * and `process()`/`process_cl()` blend it directly without per-run resampling.
   * On commit/invalidate, it is upsampled back into `base_patch`. */
  drawlayer_patch_t process_patch;
  /* Scratch buffer used to refresh only a damaged sub-rectangle of
   * `process_patch` from `base_patch`. Keeping this separate avoids
   * re-rasterizing scaled dabs directly in process-tile space, which was fast
   * but not geometrically exact once zooming in. */
  drawlayer_patch_t process_update_patch;
  /* Single-level undo snapshot of the full-resolution cached layer, swapped by reference. */
  drawlayer_patch_t undo_patch;
  /* `stroke_mask` stores the current stroke's own accumulated alpha in full layer
   * coordinates. It is reset at stroke start and lets flow=0 reason about "no
   * self build-up" against the current stroke only, not against the pre-existing
   * destination alpha already on the canvas. */
  float *stroke_mask;
  int stroke_mask_width;
  int stroke_mask_height;
  /* Stroke-local alpha mask for `process_patch`, so flow=0 remains consistent
   * when the backend worker updates the visible source crop directly. */
  float *process_stroke_mask;
  int process_stroke_mask_width;
  int process_stroke_mask_height;
  /* `live_patch` currently tracks the visible layer-space rectangle only. */
  drawlayer_patch_t live_patch;
  /* `live_argb`/`live_surface` hold the transient widget overlay painted in gui_post_expose(). */
  unsigned char *live_argb;
  cairo_surface_t *live_surface;
  /* Stroke-local alpha mask for the preview overlay, so flow=0 uses the same
   * stroke-only cap semantics in GUI feedback as in the backend cache. */
  float *preview_stroke_mask;
  int preview_stroke_mask_width;
  int preview_stroke_mask_height;
  /* Byte stride of `live_argb` in cairo ARGB32 layout. */
  int live_stride;
  /* Last brush mode used to render the overlay; helps keep preview state coherent. */
  int live_mode;
  /* True once the current overlay contains any uncommitted stroke data. */
  gboolean live_dirty;
  /* Dirty flag for the cairo-side mirror of the live overlay buffer. */
  gboolean live_surface_dirty;
  /* Damage rectangle pending upload from the overlay buffer into the cairo surface. */
  gboolean live_surface_damage_valid;
  int live_surface_damage_x0;
  int live_surface_damage_y0;
  int live_surface_damage_x1;
  int live_surface_damage_y1;
  /* Whole-stroke damage in widget coordinates, used to clear only the touched area. */
  gboolean stroke_damage_valid;
  int stroke_damage_x0;
  int stroke_damage_y0;
  int stroke_damage_x1;
  int stroke_damage_y1;
  /* Whether `base_patch` currently mirrors the selected TIFF layer successfully. */
  gboolean cache_valid;
  /* Whether `base_patch` has local edits not yet flushed back to the sidecar TIFF. */
  gboolean cache_dirty;
  /* Availability flags for the single-level undo/redo swap buffers. */
  gboolean undo_available;
  gboolean redo_available;
  /* Image/layer identity cached with `base_patch`, so the cache can be invalidated on change. */
  int32_t cache_imgid;
  int cache_raw_width;
  int cache_raw_height;
  char cache_layer_name[DRAWLAYER_NAME_SIZE];
  int cache_layer_order;
  /* Extra retained references held on top of the base patch's ownership ref.
   * These make the authoritative cache line intentionally "sticky" in the
   * global pixelpipe cache:
   * - one extra ref when the base patch is first materialized from TIFF,
   * - one extra ref for each committed GUI stroke since the last explicit save.
   * Saving the sidecar releases those extra refs again. */
  gboolean base_patch_loaded_ref;
  uint32_t base_patch_stroke_refs;
  /* Private runtime revision of `base_patch` content. Unlike the serialized
   * history hash, this exists only to invalidate internal derived caches (such
   * as the post-zoom process tile cache) when the in-memory layer changes. */
  uint64_t cache_epoch;
  /* Geometry signature and affine mapping for `process_patch`. Unlike
   * `cache_epoch`, this tracks only scale/translation geometry changes; content
   * changes are applied directly into the visible source crop by the backend
   * worker. */
  gboolean process_patch_valid;
  /* True when `process_patch` contains edits not yet folded back into
   * `base_patch`. This lets flush paths skip expensive resampling when the
   * transformed tile only mirrors base content. */
  gboolean process_patch_dirty;
  /* Union of unsynced process-tile edits, expressed in base/layer coordinates.
   * Flushing only this rectangle avoids full-tile upsampling on every sync. */
  gboolean process_dirty_bounds_valid;
  int process_dirty_x0;
  int process_dirty_y0;
  int process_dirty_x1;
  int process_dirty_y1;
  uint64_t process_geom_hash;
  dt_iop_roi_t process_combined_roi;
  /* Per-stroke damage bounds in both layer and widget spaces. */
  drawlayer_stroke_t stroke;
  /* Opaque realtime worker state owned by realtime.c. This hides the worker
   * threads, ring buffers, synchronization primitives, and async callback ids
   * from the GUI/controller layer. */
  drawlayer_rt_state_t *rt;
  /* Number of raw samples queued for the current stroke, used for history/hash bookkeeping. */
  guint stroke_sample_count;
  /* Keep the transient overlay visible until the UI pipe has recomputed after commit. */
  gboolean preview_drop_pending;
  /* A stroke has ended and history commit should happen once workers are drained. */
  gboolean finish_commit_pending;
  /* Backend worker has already applied the current stroke into `base_patch`. */
  gboolean pending_stroke_applied;
  /* Monotonic stroke ids so overlapping queued strokes can be disambiguated safely. */
  uint32_t current_stroke_batch;
  uint32_t next_stroke_batch;
  /* GUI-only resync debounce stays in the controller layer because it is not
   * tied to the worker queues. Worker-coupled async ids live in realtime.c. */
  gboolean resync_pending;
  gboolean resync_record_history;
  guint resync_source_id;
  /* GUI-only preview mode: image / solid white / solid grey / solid black. */
  int preview_bg_mode;
  /* Suppress repeated "missing layer" prompts while the same missing name stays selected. */
  gboolean missing_layer_prompted;
  char missing_layer_prompt_name[DRAWLAYER_NAME_SIZE];
  /* Custom color picker state in a plane orthogonal to the RGB black/white
   * diagonal. `picker_m` is the achromatic coordinate (mean RGB), `picker_u/v`
   * are the two opponent-plane coordinates, and `picker_hue/chroma` cache the
   * polar form of `u/v` for the right-hand m/chroma plane. The persisted config
   * remains display RGB. */
  float picker_m;
  float picker_u;
  float picker_v;
  float picker_hue;
  float picker_chroma;
  int picker_drag_mode;
  float color_history[DRAWLAYER_COLOR_HISTORY_COUNT][3];
  gboolean color_history_valid[DRAWLAYER_COLOR_HISTORY_COUNT];
  /* Cached rasterized surface for the custom color picker. Rebuilt only when
   * the picker state, widget size or output pixel density changes. */
  cairo_surface_t *color_surface;
  int color_surface_width;
  int color_surface_height;
  double color_surface_ppd;
  gboolean color_surface_dirty;
  /* Cached cursor stamp preview painted in gui_post_expose(). It mirrors the
   * current brush fall-off, display color and opacity at the current on-screen
   * brush radius, so the cursor shows a filled stamp instead of only an outline. */
  cairo_surface_t *cursor_surface;
  int cursor_surface_size;
  double cursor_surface_ppd;
  float cursor_radius;
  float cursor_opacity;
  float cursor_hardness;
  int cursor_shape;
  float cursor_color[3];

  /* Brush controls (GUI-only; persisted through dt_conf, not module params). */
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
  GtkWidget *bristles;
  GtkWidget *bristle_size;
  GtkWidget *sprinkles;
  GtkWidget *sprinkle_size;
  GtkWidget *softness;
  GtkWidget *hdr_exposure;
  /* Layer selection / layer actions. */
  GtkEntry *layer_name;
  GtkWidget *layer_select;
  GtkWidget *undo_button;
  GtkWidget *redo_button;
  GtkWidget *preview_bg_image;
  GtkWidget *preview_bg_white;
  GtkWidget *preview_bg_grey;
  GtkWidget *preview_bg_black;
  GtkWidget *delete_layer;
  GtkWidget *create_layer;
  GtkWidget *create_background;
  GtkWidget *save_layer;
  GtkWidget *fill_white;
  GtkWidget *fill_black;
  GtkWidget *fill_transparent;

  /* Optional stylus mappings for pressure. */
  GtkWidget *map_pressure_size;
  GtkWidget *map_pressure_opacity;
  GtkWidget *map_pressure_flow;
  GtkWidget *map_pressure_softness;

  /* Optional stylus mappings for tilt. */
  GtkWidget *map_tilt_size;
  GtkWidget *map_tilt_opacity;
  GtkWidget *map_tilt_flow;
  GtkWidget *map_tilt_softness;

  /* Optional stylus mappings for pointer acceleration. */
  GtkWidget *map_accel_size;
  GtkWidget *map_accel_opacity;
  GtkWidget *map_accel_flow;
  GtkWidget *map_accel_softness;

  GtkWidget *pressure_profile;
  GtkWidget *tilt_profile;
  GtkWidget *accel_profile;
  gboolean background_job_running;
} dt_iop_drawlayer_gui_data_t;

typedef struct dt_iop_drawlayer_data_t
{
  /* Keep serialized params as the first field so the pipe runtime can mirror the
   * module params while also carrying headless-only caches for non-GUI pipes. */
  dt_iop_drawlayer_params_t params;

  /* Non-GUI pipelines (thumbnail/export/headless) do not have `gui_data`, so
   * they need their own in-memory authoritative layer cache loaded outside of
   * `process()`. This is loaded in `commit_params()` and then reused read-only
   * by `process()`. */
  drawlayer_patch_t headless_base_patch;
  gboolean headless_cache_valid;
  int32_t headless_cache_imgid;
  char headless_cache_layer_name[DRAWLAYER_NAME_SIZE];
  int headless_cache_layer_order;
} dt_iop_drawlayer_data_t;

#ifdef HAVE_OPENCL
typedef struct dt_iop_drawlayer_global_data_t
{
  int kernel_premult_over;
} dt_iop_drawlayer_global_data_t;
#endif

typedef union fp32_t
{
  uint32_t u;
  float f;
} fp32_t;

void gui_update(dt_iop_module_t *self);
static gboolean _commit_dabs(dt_iop_module_t *self, const gboolean record_history);
static gboolean _checkpoint_stroke_history(dt_iop_module_t *self);
static void _union_damage_rect(gboolean *valid, int *x0, int *y0, int *x1, int *y1,
                               const int add_x0, const int add_y0, const int add_x1, const int add_y1);
static gboolean _flush_layer_cache(dt_iop_module_t *self);
static gboolean _compute_view_patch(dt_iop_module_t *self, const float padding, drawlayer_patch_t *patch,
                                    float *x0, float *y0, float *x1, float *y1);
static gboolean _is_drawlayer_display_pipe(const dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece);
static void _invalidate_process_patch(dt_iop_drawlayer_gui_data_t *g);
static gboolean _ensure_process_patch_buffer(dt_iop_drawlayer_gui_data_t *g, const int width, const int height);
static gboolean _ensure_process_update_patch_buffer(dt_iop_drawlayer_gui_data_t *g, const int width, const int height);
static void _flush_process_patch_to_base(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g);
static gboolean G_GNUC_UNUSED _refresh_process_patch_from_base_damage(dt_iop_drawlayer_gui_data_t *g,
                                                                      const int src_x0, const int src_y0,
                                                                      const int src_x1, const int src_y1);
static gboolean _sync_temp_buffers(dt_iop_module_t *self, const gboolean flush_pending, const gboolean record_history);
static gboolean _layer_bounds_to_widget_bounds(dt_iop_module_t *self, const float x0, const float y0,
                                               const float x1, const float y1,
                                               float *left, float *top, float *right, float *bottom);
static float _widget_brush_radius(dt_iop_module_t *self, const drawlayer_dab_t *dab, const float fallback);
static gboolean _layer_to_widget_coords(dt_iop_module_t *self, const float x, const float y, float *wx, float *wy);
static void _develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data);
static void _sync_mode_sensitive_widgets(dt_iop_module_t *self);
static void _accumulate_stroke_dab_analytic(float *buffer, const int width, const int height, const int origin_x,
                                            const int origin_y, const float scale, const drawlayer_dab_t *dab,
                                            const float sample_opacity_scale, float *stroke_mask,
                                            const int stroke_mask_width, const int stroke_mask_height,
                                            drawlayer_rt_path_state_t *path_state);
static void _accumulate_preview_dab_analytic(unsigned char *buffer, const int stride, const int width, const int height,
                                             const drawlayer_dab_t *dab, const float sample_opacity_scale,
                                             float *stroke_mask, const int stroke_mask_width,
                                             const int stroke_mask_height, drawlayer_rt_path_state_t *path_state);
static gboolean _wait_worker_idle(dt_iop_module_t *self);
static gboolean _layerio_sidecar_path(const int32_t imgid, char *path, const size_t path_size);
static gboolean _layerio_find_layer(const char *path, const char *layer_name, const int preferred_index,
                                    drawlayer_dir_info_t *out_info);
static gboolean _layerio_load_layer(const char *path, const char *layer_name, const int preferred_index,
                                    const int width, const int height, drawlayer_patch_t *patch);
static void _rt_init_state(dt_iop_drawlayer_gui_data_t *g);
static void _rt_cleanup_state(dt_iop_drawlayer_gui_data_t *g);
static gboolean _rt_workers_active(dt_iop_drawlayer_gui_data_t *g);
static void _rt_schedule_async_redraw(dt_iop_module_t *self);
static void _rt_clear_backend_status_active(dt_iop_drawlayer_gui_data_t *g);
static void _rt_schedule_async_undo_ui(dt_iop_module_t *self);
static void _rt_cancel_async_undo_ui(dt_iop_module_t *self);
static gboolean _start_worker(dt_iop_module_t *self);
static void _stop_worker(dt_iop_module_t *self);
static void _pause_worker(dt_iop_module_t *self);
static void _resume_worker(dt_iop_module_t *self);
static gboolean _enqueue_input(dt_iop_module_t *self, const drawlayer_raw_input_t *input);
static gboolean _enqueue_stroke_end(dt_iop_module_t *self);
static void _process_live_input(dt_iop_module_t *self, const drawlayer_raw_input_t *input);
static void _process_backend_input(dt_iop_module_t *self, const drawlayer_raw_input_t *input);
static void _process_live_dab(dt_iop_module_t *self, const drawlayer_dab_t *dab);
static void _process_backend_dab(dt_iop_module_t *self, const drawlayer_dab_t *dab);
static void _reset_input_path_state(drawlayer_rt_path_state_t *state);
static gboolean _async_redraw_idle(gpointer user_data);
static gboolean _async_backend_status_idle(gpointer user_data);
static void _cancel_async_redraw(dt_iop_module_t *self);
static void _cancel_async_backend_status(dt_iop_module_t *self);
static void _cancel_async_history_tick(dt_iop_module_t *self);
static void _cancel_async_commit(dt_iop_module_t *self);
static void _cancel_async_resync(dt_iop_module_t *self);
static gboolean _async_commit_idle(gpointer user_data);
static float _conf_smoothing(void);
static gboolean _fill_current_layer(dt_iop_module_t *self, const float value);
static gboolean _clear_current_layer(dt_iop_module_t *self);
static void _paint_clear_stroke_state(drawlayer_stroke_t *stroke);
static void _paint_reset_stroke_runtime(dt_iop_drawlayer_gui_data_t *g);
static void _populate_layer_list(dt_iop_module_t *self);
static gboolean _create_new_layer(dt_iop_module_t *self, const char *requested_name);
static int _offer_missing_layer_recreation(dt_iop_module_t *self, const char *missing_name);
static gboolean _current_layer_missing_in_sidecar(dt_iop_module_t *self);
static gboolean _create_background_layer_from_input(dt_iop_module_t *self);
static void _create_background_clicked(GtkButton *button, gpointer user_data);
static void _save_layer_clicked(GtkButton *button, gpointer user_data);
static void _invalidate_undo_redo(dt_iop_module_t *self);
static void _sync_undo_redo_buttons(dt_iop_module_t *self);
static void _sync_save_button(dt_iop_module_t *self);
static gboolean _prepare_undo_snapshot(dt_iop_module_t *self);
static gboolean _swap_undo_redo(dt_iop_module_t *self, const gboolean undo);
static void _sync_preview_bg_buttons(dt_iop_module_t *self);
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece);
static void _clear_patch(drawlayer_patch_t *patch);
static int32_t _background_layer_job_run(dt_job_t *job);
static gboolean _background_layer_job_done_idle(gpointer user_data);
static void _clear_cursor_stamp_surface(dt_iop_drawlayer_gui_data_t *g);
static void _sync_picker_from_display_rgb(dt_iop_drawlayer_gui_data_t *g, const float display_rgb[3]);

static gboolean _is_drawlayer_display_pipe(const dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece)
{
  if(!self || !self->dev || !piece || !piece->pipe) return FALSE;
  if(self->dev->gui_module != self) return FALSE;
  /* Displayed darkroom rendering can come from either `dev->pipe` or
   * `dev->preview_pipe` depending on current scheduling/state. Both must use
   * the GUI-owned process patch fast path to avoid falling back to the
   * headless full-res clip+zoom path (slow and stale during live strokes). */
  return (piece->pipe == self->dev->pipe || piece->pipe == self->dev->preview_pipe);
}
static gboolean _alloc_cache_patch(drawlayer_patch_t *patch, const uint64_t hash, const size_t pixel_count,
                                   const int width, const int height, const char *name, int *created_out);
static void *_alloc_tracked_temp_buffer(size_t bytes, const char *name);
static void _free_tracked_temp_buffer(void **buffer, const char *name);
static void _effective_buf_in_origin(const dt_dev_pixelpipe_iop_t *piece, const int current_full_w,
                                     const int current_full_h, int *origin_x, int *origin_y);
static void _touch_layer_cache_epoch(dt_iop_drawlayer_gui_data_t *g);
static uint64_t _drawlayer_params_cache_hash(const int32_t imgid, const dt_iop_drawlayer_params_t *params);
static gboolean _rekey_shared_base_patch(drawlayer_patch_t *patch, const int32_t imgid,
                                         const dt_iop_drawlayer_params_t *params);
static void _retain_base_patch_loaded_ref(dt_iop_drawlayer_gui_data_t *g);
static void _retain_base_patch_stroke_ref(dt_iop_drawlayer_gui_data_t *g);
static void _release_all_base_patch_extra_refs(dt_iop_drawlayer_gui_data_t *g);

static inline float _clamp01(const float value)
{
  return fminf(fmaxf(value, 0.0f), 1.0f);
}

static inline void _set_toggle_if_valid(GtkWidget *widget, const gboolean active)
{
  if(GTK_IS_TOGGLE_BUTTON(widget)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), active);
}

static inline float _lerpf(const float a, const float b, const float t)
{
  return a + (b - a) * t;
}

static inline float _cubic_hermitef(const float p0, const float p1, const float m0, const float m1, const float t)
{
  const float t2 = t * t;
  const float t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * m0
         + (-2.0f * t3 + 3.0f * t2) * p1 + (t3 - t2) * m1;
}

static drawlayer_dab_t _sample_uniform_history_dab(const drawlayer_dab_t *samples, const int count, const float index)
{
  /* The stroke history is already uniformly sampled in space. This helper reconstructs
   * an anchor sample at an arbitrary fractional sample index, so higher-level logic can
   * talk in arc length instead of in raw event counts. */
  if(!samples || count <= 0) return (drawlayer_dab_t){ 0 };
  if(count == 1) return samples[0];

  const float clamped = CLAMP(index, 0.0f, (float)(count - 1));
  const int i0 = CLAMP((int)floorf(clamped), 0, count - 1);
  const int i1 = CLAMP(i0 + 1, 0, count - 1);
  if(i0 == i1) return samples[i0];

  const float t = clamped - (float)i0;
  drawlayer_dab_t out = samples[i0];
  out.x = _lerpf(samples[i0].x, samples[i1].x, t);
  out.y = _lerpf(samples[i0].y, samples[i1].y, t);
  out.wx = _lerpf(samples[i0].wx, samples[i1].wx, t);
  out.wy = _lerpf(samples[i0].wy, samples[i1].wy, t);
  out.radius = _lerpf(samples[i0].radius, samples[i1].radius, t);
  {
    const float dir_x = _lerpf(samples[i0].dir_x, samples[i1].dir_x, t);
    const float dir_y = _lerpf(samples[i0].dir_y, samples[i1].dir_y, t);
    const float dir_len = hypotf(dir_x, dir_y);
    if(dir_len > 1e-6f)
    {
      out.dir_x = dir_x / dir_len;
      out.dir_y = dir_y / dir_len;
    }
    else
    {
      out.dir_x = 0.0f;
      out.dir_y = 1.0f;
    }
  }
  out.opacity = _lerpf(samples[i0].opacity, samples[i1].opacity, t);
  out.flow = _lerpf(samples[i0].flow, samples[i1].flow, t);
  out.bristles = _lerpf(samples[i0].bristles, samples[i1].bristles, t);
  out.sprinkles = _lerpf(samples[i0].sprinkles, samples[i1].sprinkles, t);
  out.softness = _lerpf(samples[i0].softness, samples[i1].softness, t);
  for(int c = 0; c < 3; c++)
  {
    out.color[c] = _lerpf(samples[i0].color[c], samples[i1].color[c], t);
    out.display_color[c] = _lerpf(samples[i0].display_color[c], samples[i1].display_color[c], t);
  }
  out.shape = (t < 0.5f) ? samples[i0].shape : samples[i1].shape;
  out.mode = (t < 0.5f) ? samples[i0].mode : samples[i1].mode;
  return out;
}

static void _apply_input_smoothing(dt_iop_module_t *self, GArray *history, drawlayer_dab_t *dab,
                                   const float sample_spacing, const float radius)
{
  /* Smoothing is intentionally kept simple and explicitly tied to brush scale, not to the
   * number of incoming GUI events. The kept implementation:
   * - finds two anchor samples in the already-uniform stroke history,
   * - keeps them one brush radius apart in arc length,
   * - linearly extrapolates the current cursor from that chord,
   * - blends the raw cursor toward that prediction.
   *
   * More elaborate variants were tried here (fixed-N windows, higher-order fits, inertial
   * heading filters). They benchmarked worse, reintroduced sampling dependence, or created
   * visible cusps/overshoot. This simpler predictor is the current compromise. */
  if(!history || !dab || history->len < 2) return;

  const float smoothing = _conf_smoothing() / 100.0f;
  if(smoothing <= 0.0f) return;

  const drawlayer_dab_t *samples = (const drawlayer_dab_t *)history->data;
  const int count = (int)history->len;
  const float step = fmaxf(sample_spacing, 1e-6f);
  const float anchor_radius = fmaxf(radius, 0.5f);
  const float anchor_span = anchor_radius / step;
  if(anchor_span <= 1e-6f) return;

  const float last_index = (float)(count - 1);
  const float prev_index = last_index - anchor_span;
  if(prev_index < 0.0f) return;

  /* Anchor spacing is defined in physical path length (one radius), not in "N samples".
   * Fractional indexing keeps this invariant when the user changes `distance`. */
  const drawlayer_dab_t anchor0 = _sample_uniform_history_dab(samples, count, prev_index);
  const drawlayer_dab_t anchor2 = _sample_uniform_history_dab(samples, count, last_index);

  const float advance = hypotf(dab->x - anchor2.x, dab->y - anchor2.y) / anchor_radius;
  const float slope_x = anchor2.x - anchor0.x;
  const float slope_y = anchor2.y - anchor0.y;
  const float pred_x = anchor2.x + fmaxf(advance, 0.0f) * slope_x;
  const float pred_y = anchor2.y + fmaxf(advance, 0.0f) * slope_y;

  const float blend = 0.5f * smoothing;
  dab->x = _lerpf(dab->x, pred_x, blend);
  dab->y = _lerpf(dab->y, pred_y, blend);

  if(!_layer_to_widget_coords(self, dab->x, dab->y, &dab->wx, &dab->wy))
  {
    /* Fall back to the raw widget coordinates if the forward transform is unavailable.
     * Keeping one authoritative smoothed geometry (layer space) avoids GUI/pipeline drift. */
  }
}

static inline float _stroke_union_effective_alpha(const float dst_alpha, const float src_alpha)
{
  const float old_alpha = _clamp01(dst_alpha);
  const float target_alpha = fmaxf(old_alpha, _clamp01(src_alpha));

  if(target_alpha <= old_alpha + 1e-8f || old_alpha >= 1.0f - 1e-8f) return 0.0f;
  return _clamp01((target_alpha - old_alpha) / (1.0f - old_alpha));
}

static inline float _stroke_union_effective_erase_alpha(const float dst_alpha, const float src_alpha)
{
  /* Erase uses multiplicative alpha removal: dst' = dst * (1 - src).
   * The "no self-build-up" equivalent therefore caps a whole stroke to the alpha reduction
   * of a single brush imprint. Once a pixel is already below that cap, more dabs at flow=0
   * must not keep erasing it. */
  const float old_alpha = _clamp01(dst_alpha);
  const float brush_alpha = _clamp01(src_alpha);

  if(old_alpha <= 1e-8f || brush_alpha <= 1e-8f) return 0.0f;

  const float target_alpha = fminf(old_alpha, 1.0f - brush_alpha * old_alpha);
  if(target_alpha >= old_alpha - 1e-8f) return 0.0f;
  return _clamp01(1.0f - target_alpha / old_alpha);
}

static inline float _half_to_float(const uint16_t h)
{
  static const fp32_t magic = { 113u << 23 };
  static const uint32_t shifted_exp = 0x7c00u << 13;
  fp32_t out;

  out.u = (h & 0x7fffu) << 13;
  const uint32_t exp = shifted_exp & out.u;
  out.u += (127u - 15u) << 23;

  if(exp == shifted_exp)
    out.u += (128u - 16u) << 23;
  else if(exp == 0)
  {
    out.u += 1u << 23;
    out.f -= magic.f;
  }

  out.u |= (h & 0x8000u) << 16;
  return out.f;
}

static inline uint16_t _float_to_half(float value)
{
  fp32_t in = { .f = value };
  const uint32_t sign = (in.u >> 16) & 0x8000u;
  const uint32_t exponent = (in.u >> 23) & 0xffu;
  uint32_t mantissa = in.u & 0x007fffffu;

  if(exponent == 0xffu)
  {
    if(mantissa) return (uint16_t)(sign | 0x7e00u);
    return (uint16_t)(sign | 0x7c00u);
  }

  const int32_t half_exponent = (int32_t)exponent - 127 + 15;

  if(half_exponent >= 0x1f) return (uint16_t)(sign | 0x7c00u);
  if(half_exponent <= 0)
  {
    if(half_exponent < -10) return (uint16_t)sign;

    mantissa |= 0x00800000u;
    const uint32_t shift = (uint32_t)(1 - half_exponent);
    uint32_t half_mantissa = mantissa >> (shift + 13);
    const uint32_t round_bit = 1u << (shift + 12);

    if((mantissa & round_bit) && ((mantissa & (round_bit - 1u)) || (half_mantissa & 1u)))
      half_mantissa++;

    return (uint16_t)(sign | half_mantissa);
  }

  uint32_t half_exp = (uint32_t)half_exponent << 10;
  uint32_t half_mantissa = mantissa >> 13;

  if((mantissa & 0x00001000u) && ((mantissa & 0x00001fffu) || (half_mantissa & 1u)))
  {
    half_mantissa++;

    if(half_mantissa == 0x0400u)
    {
      half_mantissa = 0;
      half_exp += 0x0400u;
      if(half_exp >= 0x7c00u) return (uint16_t)(sign | 0x7c00u);
    }
  }

  return (uint16_t)(sign | half_exp | half_mantissa);
}

static void _default_layer_name(dt_iop_module_t *self, char *name, const size_t name_size)
{
  const char *suffix = "0";
  if(self && self->multi_name[0] != '\0') suffix = self->multi_name;
  else if(self)
  {
    static char fallback[16];
    g_snprintf(fallback, sizeof(fallback), "%d", MAX(self->multi_priority, 0) + 1);
    suffix = fallback;
  }
  g_snprintf(name, name_size, "layer %s", suffix);
}

static gboolean _layer_name_non_empty(const char *name)
{
  if(!name) return FALSE;
  char tmp[DRAWLAYER_NAME_SIZE] = { 0 };
  g_strlcpy(tmp, name, sizeof(tmp));
  g_strstrip(tmp);
  return tmp[0] != '\0';
}

static void _work_profile_key_from_info(const dt_iop_order_iccprofile_info_t *info, char *key, const size_t key_size)
{
  if(!key || key_size == 0) return;
  key[0] = '\0';
  if(!info) return;

  g_snprintf(key, key_size, "%d|%d|%s", (int)info->type, (int)info->intent, info->filename);
}

static gboolean _get_current_work_profile_key(dt_iop_module_t *self, GList *iop_list, dt_dev_pixelpipe_t *pipe,
                                              char *key, const size_t key_size)
{
  if(!key || key_size == 0) return FALSE;
  key[0] = '\0';
  if(!self || !pipe || !iop_list) return FALSE;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(self, iop_list);
  if(!work_profile) return FALSE;

  _work_profile_key_from_info(work_profile, key, key_size);
  return key[0] != '\0';
}

static void _ensure_layer_name(dt_iop_module_t *self, dt_iop_drawlayer_params_t *params)
{
  if(params->layer_name[0] != '\0') return;
  _default_layer_name(self, params->layer_name, sizeof(params->layer_name));
  params->layer_order = -1;
}

static void _clear_transparent_half(uint16_t *pixels, const size_t pixel_count)
{
  if(!pixels) return;
  memset(pixels, 0, pixel_count * 4 * sizeof(uint16_t));
}

static void _clear_transparent_float(float *pixels, const size_t pixel_count)
{
  if(!pixels) return;
  memset(pixels, 0, pixel_count * 4 * sizeof(float));
}

typedef struct drawlayer_process_scratch_t
{
  float *fit_canvas;
  size_t fit_canvas_pixels;
  float *fit_scaled;
  size_t fit_scaled_pixels;
  float *module_input;
  size_t module_input_pixels;
  float *layerbuf;
  size_t layerbuf_pixels;
  float *cl_layer_rgba;
  size_t cl_layer_rgba_pixels;
  float *cl_layer_alpha;
  size_t cl_layer_alpha_pixels;
  float *cl_background_rgba;
  size_t cl_background_rgba_pixels;
} drawlayer_process_scratch_t;

static G_GNUC_UNUSED void _destroy_process_scratch(gpointer data)
{
  drawlayer_process_scratch_t *scratch = (drawlayer_process_scratch_t *)data;
  if(!scratch) return;
  _free_tracked_temp_buffer((void **)&scratch->fit_canvas, "drawlayer process scratch");
  _free_tracked_temp_buffer((void **)&scratch->fit_scaled, "drawlayer process scratch");
  _free_tracked_temp_buffer((void **)&scratch->module_input, "drawlayer process scratch");
  _free_tracked_temp_buffer((void **)&scratch->layerbuf, "drawlayer process scratch");
  _free_tracked_temp_buffer((void **)&scratch->cl_layer_rgba, "drawlayer process scratch");
  _free_tracked_temp_buffer((void **)&scratch->cl_layer_alpha, "drawlayer process scratch");
  _free_tracked_temp_buffer((void **)&scratch->cl_background_rgba, "drawlayer process scratch");
  g_free(scratch);
}

static G_GNUC_UNUSED GPrivate _drawlayer_process_scratch_key = G_PRIVATE_INIT(_destroy_process_scratch);

static G_GNUC_UNUSED drawlayer_process_scratch_t *_get_process_scratch(void)
{
  drawlayer_process_scratch_t *scratch = (drawlayer_process_scratch_t *)g_private_get(&_drawlayer_process_scratch_key);
  if(scratch) return scratch;

  scratch = g_malloc0(sizeof(*scratch));
  if(!scratch) return NULL;
  g_private_set(&_drawlayer_process_scratch_key, scratch);
  return scratch;
}

static void *_alloc_tracked_temp_buffer(const size_t bytes, const char *name)
{
  if(bytes == 0) return NULL;
  return dt_pixelpipe_cache_alloc_align_cache_impl(darktable.pixelpipe_cache, bytes, DT_DEV_PIXELPIPE_NONE, name);
}

static void _free_tracked_temp_buffer(void **buffer, const char *name)
{
  if(!buffer || !*buffer) return;
  dt_pixelpipe_cache_free_align_cache(darktable.pixelpipe_cache, buffer, name);
}

static G_GNUC_UNUSED float *_ensure_process_scratch_buffer(float **buffer, size_t *capacity_pixels, const size_t needed_pixels)
{
  if(!buffer || !capacity_pixels || needed_pixels == 0) return NULL;
  if(*capacity_pixels < needed_pixels)
  {
    _free_tracked_temp_buffer((void **)buffer, "drawlayer process scratch");
    float *new_buffer = _alloc_tracked_temp_buffer(needed_pixels * 4 * sizeof(float), "drawlayer process scratch");
    if(!new_buffer) return NULL;
    *buffer = new_buffer;
    *capacity_pixels = needed_pixels;
  }
  return *buffer;
}

static G_GNUC_UNUSED float *_ensure_process_scratch_scalar_buffer(float **buffer, size_t *capacity_pixels,
                                                                  const size_t needed_pixels)
{
  if(!buffer || !capacity_pixels || needed_pixels == 0) return NULL;
  if(*capacity_pixels < needed_pixels)
  {
    _free_tracked_temp_buffer((void **)buffer, "drawlayer process scratch");
    float *new_buffer = _alloc_tracked_temp_buffer(needed_pixels * sizeof(float), "drawlayer process scratch");
    if(!new_buffer) return NULL;
    *buffer = new_buffer;
    *capacity_pixels = needed_pixels;
  }
  return *buffer;
}

static uint64_t _drawlayer_sidecar_cache_hash(const char *path, const char *layer_name, const char *purpose)
{
  uint64_t hash = 5381u;
  if(path) hash = dt_hash(hash, path, strlen(path));
  if(layer_name) hash = dt_hash(hash, layer_name, strlen(layer_name));
  if(purpose) hash = dt_hash(hash, purpose, strlen(purpose));
  return hash ? hash : 1u;
}

static uint64_t _drawlayer_params_cache_hash(const int32_t imgid, const dt_iop_drawlayer_params_t *params)
{
  /* Internal drawlayer base-cache identity must stay stable across transient
   * stroke/hash updates. Key only by image + layer identity + working profile,
   * not by volatile fields (stroke hash, sidecar timestamp...).
   *
   * This keeps the shared base patch line hot across interactive drawing ticks
   * and avoids expensive rekey conflicts/republishing during realtime updates. */
  uint64_t hash = 5381u;
  hash = dt_hash(hash, (const char *)&imgid, sizeof(imgid));
  if(params)
  {
    hash = dt_hash(hash, params->layer_name, sizeof(params->layer_name));
    hash = dt_hash(hash, (const char *)&params->layer_order, sizeof(params->layer_order));
    hash = dt_hash(hash, params->work_profile, sizeof(params->work_profile));
  }
  return hash ? hash : 1u;
}

static uint64_t _drawlayer_process_cache_hash(const dt_dev_pixelpipe_iop_t *piece,
                                              const dt_iop_roi_t *roi_in,
                                              const dt_iop_roi_t *roi_out)
{
  /* This hash identifies only the affine geometry of the transformed process
   * tile:
   * - current pipeline full-canvas size,
   * - propagated module input/output rectangles,
   * - current tile ROI.
   *
   * Content changes are intentionally *not* part of this key. The transformed
   * tile is updated incrementally by the backend worker as the stroke evolves,
   * so changing brush content should not force a full-tile resample. */
  uint64_t hash = 5381u;
  if(piece)
  {
    hash = dt_hash(hash, (const char *)&piece->pipe->image.id, sizeof(piece->pipe->image.id));
    hash = dt_hash(hash, (const char *)&piece->pipe->iwidth, sizeof(piece->pipe->iwidth));
    hash = dt_hash(hash, (const char *)&piece->pipe->iheight, sizeof(piece->pipe->iheight));
    hash = dt_hash(hash, (const char *)&piece->buf_in, sizeof(piece->buf_in));
    hash = dt_hash(hash, (const char *)&piece->buf_out, sizeof(piece->buf_out));
  }

  if(roi_in) hash = dt_hash(hash, (const char *)roi_in, sizeof(*roi_in));
  if(roi_out) hash = dt_hash(hash, (const char *)roi_out, sizeof(*roi_out));
  return hash ? hash : 1u;
}

static void _touch_layer_cache_epoch(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  g->cache_epoch++;
  if(g->cache_epoch == 0) g->cache_epoch++;
}

static inline void _patch_rdlock(const drawlayer_patch_t *patch)
{
  if(!patch || !patch->cache_entry) return;
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, patch->cache_hash, TRUE, patch->cache_entry);
}

static inline void _patch_rdunlock(const drawlayer_patch_t *patch)
{
  if(!patch || !patch->cache_entry) return;
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, patch->cache_hash, FALSE, patch->cache_entry);
}

static inline void _patch_wrlock(const drawlayer_patch_t *patch)
{
  if(!patch || !patch->cache_entry) return;
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, patch->cache_hash, TRUE, patch->cache_entry);
}

static inline void _patch_wrunlock(const drawlayer_patch_t *patch)
{
  if(!patch || !patch->cache_entry) return;
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, patch->cache_hash, FALSE, patch->cache_entry);
}

static gboolean _rekey_shared_base_patch(drawlayer_patch_t *patch, const int32_t imgid,
                                         const dt_iop_drawlayer_params_t *params)
{
  /* Rekeying lets the same pixelpipe cache line keep its allocated storage and
   * current in-memory pixels while the serialized module hash advances to a new
   * history snapshot. This is the central piece that lets other pipelines find
   * the newest base patch through the cache instead of through GUI internals. */
  if(!patch || !patch->cache_entry || !params) return FALSE;
  const uint64_t new_hash = _drawlayer_params_cache_hash(imgid, params);
  if(new_hash == patch->cache_hash) return TRUE;
  if(dt_dev_pixelpipe_cache_rekey(darktable.pixelpipe_cache, patch->cache_hash, new_hash, patch->cache_entry) == 0)
  {
    patch->cache_hash = new_hash;
    return TRUE;
  }

  /* Fallback path: another cache line already owns `new_hash` (or rekeying
   * failed for any other reason). Publish the current authoritative pixels into
   * that target key explicitly so parallel/headless pipelines resolving by the
   * latest params hash still see up-to-date content.
   *
   * We intentionally keep `patch` bound to its original cache entry/hash here,
   * because this module may hold additional explicit refs on that entry
   * (`base_patch_loaded_ref`, stroke refs). Rebinding `patch` would desynchronize
   * those ref counters. */
  if(!patch->pixels || patch->width <= 0 || patch->height <= 0) return FALSE;

  drawlayer_patch_t published = { 0 };
  int created = 0;
  if(!_alloc_cache_patch(&published, new_hash, (size_t)patch->width * patch->height,
                         patch->width, patch->height, "drawlayer sidecar cache", &created))
    return FALSE;

  _patch_rdlock(patch);
  memcpy(published.pixels, patch->pixels, (size_t)patch->width * patch->height * 4 * sizeof(float));
  _patch_rdunlock(patch);
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, published.pixels, published.cache_entry, -1);
  _clear_patch(&published);
  dt_print(DT_DEBUG_PERF,
           "[drawlayer] cache rekey conflict old=%" PRIu64 " new=%" PRIu64 " -> published snapshot instead\n",
           patch->cache_hash, new_hash);
  return TRUE;
}

static void _retain_base_patch_loaded_ref(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->base_patch.cache_entry || g->base_patch_loaded_ref) return;
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, TRUE,
                                         g->base_patch.cache_entry);
  g->base_patch_loaded_ref = TRUE;
}

static void _retain_base_patch_stroke_ref(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->base_patch.cache_entry) return;
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, TRUE,
                                         g->base_patch.cache_entry);
  g->base_patch_stroke_refs++;
}

static void _release_all_base_patch_extra_refs(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(!g->base_patch.cache_entry)
  {
    /* Keep refcount bookkeeping state coherent even if the cache entry has
     * already been detached/cleared. This prevents stale counters from being
     * applied to a future reused `g->base_patch` entry. */
    g->base_patch_loaded_ref = FALSE;
    g->base_patch_stroke_refs = 0;
    return;
  }

  if(g->base_patch_loaded_ref)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, FALSE,
                                           g->base_patch.cache_entry);
    g->base_patch_loaded_ref = FALSE;
  }

  while(g->base_patch_stroke_refs > 0)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, FALSE,
                                           g->base_patch.cache_entry);
    g->base_patch_stroke_refs--;
  }
}

static gboolean _acquire_shared_base_patch(drawlayer_patch_t *patch, const int32_t imgid,
                                           const dt_iop_drawlayer_params_t *params, const int width,
                                           const int height, const char *name, int *created_out)
{
  /* All authoritative base-patch users (GUI and headless pipes) attach through
   * one shared pixelpipe cache line keyed by stable layer identity.
   * This helper hides whether the caller hit a hot cache line from another pipe
   * or had to instantiate a new one that still needs to be initialized. */
  if(!params) return FALSE;
  const uint64_t hash = _drawlayer_params_cache_hash(imgid, params);
  return _alloc_cache_patch(patch, hash, (size_t)width * height, width, height, name, created_out);
}

static void _clear_headless_cache(dt_iop_drawlayer_data_t *data)
{
  if(!data) return;
  _clear_patch(&data->headless_base_patch);
  data->headless_cache_valid = FALSE;
  data->headless_cache_imgid = -1;
  data->headless_cache_layer_name[0] = '\0';
  data->headless_cache_layer_order = -1;
}

static gboolean _refresh_headless_cache(dt_iop_module_t *self, dt_iop_drawlayer_data_t *data,
                                        const dt_iop_drawlayer_params_t *params, dt_dev_pixelpipe_iop_t *piece)
{
  if(!self || !data || !params || !piece) return FALSE;
  if(params->layer_name[0] == '\0')
  {
    _clear_headless_cache(data);
    return TRUE;
  }

  const uint64_t base_hash = _drawlayer_params_cache_hash(piece->pipe->image.id, params);
  const int known_width = data->headless_base_patch.width;
  const int known_height = data->headless_base_patch.height;
  if(data->headless_cache_valid && data->headless_base_patch.cache_entry
     && data->headless_base_patch.cache_hash == base_hash
     && data->headless_cache_imgid == piece->pipe->image.id
     && !g_strcmp0(data->headless_cache_layer_name, params->layer_name))
    return TRUE;

  _clear_headless_cache(data);

  if(known_width > 0 && known_height > 0)
  {
    int created = 0;
    if(!_acquire_shared_base_patch(&data->headless_base_patch, piece->pipe->image.id, params,
                                   known_width, known_height, "drawlayer headless sidecar cache", &created))
      return FALSE;

    if(!created)
    {
      data->headless_cache_valid = TRUE;
      data->headless_cache_imgid = piece->pipe->image.id;
      g_strlcpy(data->headless_cache_layer_name, params->layer_name, sizeof(data->headless_cache_layer_name));
      data->headless_cache_layer_order = params->layer_order;
      return TRUE;
    }

    char warm_path[PATH_MAX] = { 0 };
    gboolean warm_loaded = FALSE;
    if(_layerio_sidecar_path(piece->pipe->image.id, warm_path, sizeof(warm_path))
       && g_file_test(warm_path, G_FILE_TEST_EXISTS))
    {
      _patch_wrlock(&data->headless_base_patch);
      warm_loaded = _layerio_load_layer(warm_path, params->layer_name, params->layer_order, known_width,
                                        known_height, &data->headless_base_patch);
      _patch_wrunlock(&data->headless_base_patch);
    }

    if(warm_loaded)
    {
      data->headless_cache_valid = TRUE;
      data->headless_cache_imgid = piece->pipe->image.id;
      g_strlcpy(data->headless_cache_layer_name, params->layer_name, sizeof(data->headless_cache_layer_name));
      data->headless_cache_layer_order = params->layer_order;
      return TRUE;
    }

    _clear_headless_cache(data);
  }

  char path[PATH_MAX] = { 0 };
  if(!_layerio_sidecar_path(piece->pipe->image.id, path, sizeof(path))) return FALSE;
  if(!g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  drawlayer_dir_info_t info;
  if(!_layerio_find_layer(path, params->layer_name, params->layer_order, &info)
     || info.width == 0 || info.height == 0)
    return FALSE;

  int created = 0;
  if(!_acquire_shared_base_patch(&data->headless_base_patch, piece->pipe->image.id, params,
                                 (int)info.width, (int)info.height, "drawlayer headless sidecar cache",
                                 &created))
    return FALSE;

  if(created)
  {
    _patch_wrlock(&data->headless_base_patch);
    _clear_transparent_float(data->headless_base_patch.pixels, (size_t)info.width * info.height);
    const gboolean loaded = _layerio_load_layer(path, params->layer_name, info.index, (int)info.width,
                                                (int)info.height, &data->headless_base_patch);
    _patch_wrunlock(&data->headless_base_patch);
    if(!loaded)
    {
      _clear_headless_cache(data);
      return FALSE;
    }
  }

  data->headless_cache_valid = TRUE;
  data->headless_cache_imgid = piece->pipe->image.id;
  g_strlcpy(data->headless_cache_layer_name, params->layer_name, sizeof(data->headless_cache_layer_name));
  data->headless_cache_layer_order = info.index;
  return TRUE;
}

static gboolean _alloc_cache_patch(drawlayer_patch_t *patch, const uint64_t hash, const size_t pixel_count,
                                   const int width, const int height, const char *name, int *created_out)
{
  if(!patch || pixel_count == 0 || width <= 0 || height <= 0) return FALSE;

  dt_iop_buffer_dsc_t dsc = { 0 };
  dsc.channels = 4;
  dsc.datatype = TYPE_FLOAT;
  dsc.cst = IOP_CS_RGB;

  void *data = NULL;
  dt_iop_buffer_dsc_t *entry_dsc = &dsc;
  dt_pixel_cache_entry_t *entry = NULL;
  const int created = dt_dev_pixelpipe_cache_get(darktable.pixelpipe_cache, hash, pixel_count * 4 * sizeof(float),
                                                 name, DT_DEV_PIXELPIPE_NONE, TRUE, &data, &entry_dsc, &entry);
  if(created_out) *created_out = created;
  if(!data || !entry)
  {
    if(entry)
    {
      if(created) dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, FALSE, entry);
      dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, FALSE, entry);
    }
    return FALSE;
  }

  _clear_patch(patch);
  patch->x = 0;
  patch->y = 0;
  patch->width = width;
  patch->height = height;
  patch->pixels = (float *)data;
  patch->cache_entry = entry;
  patch->cache_hash = hash;
  patch->external_alloc = FALSE;

  if(created)
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, FALSE, entry);
  return TRUE;
}

/* Sizing step 1: raw-sized layer -> current pipeline full canvas.
 *
 * The TIFF sidecar is authored and stored at raw-sized full-image resolution.
 * That is the authoring/reference space for the layer contents.
 *
 * At process time, however, `piece->pipe->iwidth/iheight` describe the full-size
 * input image "here and now" for the current pipeline:
 * - darkroom full pipe: usually the uncropped processed full image,
 * - thumbnail/export pipe: often a uniformly downscaled full image.
 *
 * We therefore first fit the raw-sized sidecar canvas into the current pipeline
 * full-image basis. This is intentionally a uniform fit (preserve aspect ratio),
 * not a stretch. Thumbnail pipes use a fit, and stretching here would immediately
 * misalign the layer before any crop is applied.
 *
 * If the fitted dimensions do not exactly equal `dst_w/dst_h` because of integer
 * rounding, the fitted image is centered on a transparent full canvas of that
 * final size. Later sizing stages then work in the exact same full-canvas basis
 * as the current pipeline. */
static G_GNUC_UNUSED float *_fit_layer_to_current_canvas(drawlayer_process_scratch_t *scratch, const float *src,
                                                         const int src_w, const int src_h, const int dst_w,
                                                         const int dst_h)
{
  if(!scratch || !src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return NULL;

  const float fit = fminf((float)dst_w / (float)src_w, (float)dst_h / (float)src_h);
  const int scaled_w = MAX(1, MIN(dst_w, (int)lroundf(src_w * fit)));
  const int scaled_h = MAX(1, MIN(dst_h, (int)lroundf(src_h * fit)));
  const size_t canvas_pixels = (size_t)dst_w * dst_h;

  float *canvas = _ensure_process_scratch_buffer(&scratch->fit_canvas, &scratch->fit_canvas_pixels, canvas_pixels);
  if(!canvas) return NULL;
  _clear_transparent_float(canvas, canvas_pixels);

  float *scaled = NULL;
  const float *fitted = src;
  if(src_w != scaled_w || src_h != scaled_h)
  {
    scaled = _ensure_process_scratch_buffer(&scratch->fit_scaled, &scratch->fit_scaled_pixels,
                                            (size_t)scaled_w * scaled_h);
    if(!scaled)
      return NULL;

    const dt_iop_roi_t src_roi = {
      .x = 0,
      .y = 0,
      .width = src_w,
      .height = src_h,
      .scale = 1.0f,
    };
    const dt_iop_roi_t scaled_roi = {
      .x = 0,
      .y = 0,
      .width = scaled_w,
      .height = scaled_h,
      .scale = fmaxf(fit, 1e-6f),
    };
    dt_iop_clip_and_zoom(scaled, src, &scaled_roi, &src_roi, scaled_w, src_w);
    fitted = scaled;
  }

  const int dst_x = MAX((dst_w - scaled_w) / 2, 0);
  const int dst_y = MAX((dst_h - scaled_h) / 2, 0);
  for(int yy = 0; yy < scaled_h; yy++)
  {
    memcpy(canvas + 4 * ((size_t)(dst_y + yy) * dst_w + dst_x), fitted + 4 * ((size_t)yy * scaled_w),
           (size_t)scaled_w * 4 * sizeof(float));
  }

  return canvas;
}

static void _build_combined_process_roi(const dt_dev_pixelpipe_iop_t *piece,
                                        const dt_iop_roi_t *process_roi,
                                        const int current_full_w,
                                        const int current_full_h,
                                        const int src_w,
                                        const int src_h,
                                        dt_iop_roi_t *combined_roi)
{
  /* The old implementation materialized three affine stages:
   * 1. raw-sized layer -> uniformly fitted current full canvas
   * 2. current full canvas -> `piece->buf_in` module-input rectangle
   * 3. module-input rectangle -> current processing tile (`process_roi`)
   *
   * All three are pure scale/translation, so they can be composed analytically
   * into one final ROI in source-layer coordinates. We keep the interpolation in
   * `dt_iop_clip_and_zoom()`, but only call it once with that combined affine map.
   *
   * `dt_iop_clip_and_zoom()` expects `roi_out` in output-space units, with the
   * source coordinate recovered as `(x_out + roi_out.x) / roi_out.scale`.
   * Therefore, once the source-space origin is known, `roi_out.x/y` must be the
   * source origin multiplied by the final output scale. */
  if(!piece || !process_roi || !combined_roi || current_full_w <= 0 || current_full_h <= 0
     || src_w <= 0 || src_h <= 0)
  {
    if(combined_roi) memset(combined_roi, 0, sizeof(*combined_roi));
    return;
  }

  const float fit = fminf((float)current_full_w / (float)src_w, (float)current_full_h / (float)src_h);
  const int scaled_w = MAX(1, MIN(current_full_w, (int)lroundf(src_w * fit)));
  const int scaled_h = MAX(1, MIN(current_full_h, (int)lroundf(src_h * fit)));
  const int fit_offset_x = MAX((current_full_w - scaled_w) / 2, 0);
  const int fit_offset_y = MAX((current_full_h - scaled_h) / 2, 0);

  int module_origin_x = 0;
  int module_origin_y = 0;
  _effective_buf_in_origin(piece, current_full_w, current_full_h, &module_origin_x, &module_origin_y);

  const float inv_scale = process_roi->scale > 1e-6f ? (1.0f / process_roi->scale) : 0.0f;
  const float tile_origin_canvas_x = (float)module_origin_x + process_roi->x * inv_scale;
  const float tile_origin_canvas_y = (float)module_origin_y + process_roi->y * inv_scale;

  combined_roi->x = (int)lroundf((tile_origin_canvas_x - fit_offset_x) * process_roi->scale);
  combined_roi->y = (int)lroundf((tile_origin_canvas_y - fit_offset_y) * process_roi->scale);
  combined_roi->width = process_roi->width;
  combined_roi->height = process_roi->height;
  combined_roi->scale = process_roi->scale * fmaxf(fit, 1e-6f);
}

static gboolean _build_process_patch_from_base(dt_iop_module_t *self,
                                               dt_iop_drawlayer_gui_data_t *g,
                                               const dt_dev_pixelpipe_iop_t *piece,
                                               const dt_iop_roi_t *roi_in,
                                               const dt_iop_roi_t *roi_out)
{
  const gint64 t0 = g_get_monotonic_time();
  gint64 t = t0;
  /* Materialize (or refresh) a display-sized process tile from the raw-sized
   * authoritative cache. This keeps `process()`/`process_cl()` on a direct
   * blend path during live drawing: backend dabs update this tile in place and
   * pipeline blending no longer has to resample the full raw patch every run.
   *
   * When geometry changes invalidate this tile, `_flush_process_patch_to_base()`
   * upsamples it back into `base_patch` first so no in-flight stroke edit is
   * lost before rebuilding the tile for the new ROI. */
  if(!self || !g || !piece || !roi_out || !g->base_patch.pixels) return FALSE;
  const int current_full_w = piece->pipe->iwidth;
  const int current_full_h = piece->pipe->iheight;
  if(current_full_w <= 0 || current_full_h <= 0) return FALSE;

  const dt_iop_roi_t process_roi = roi_in ? *roi_in : *roi_out;
  dt_iop_roi_t combined_roi = { 0 };
  _build_combined_process_roi(piece, &process_roi, current_full_w, current_full_h,
                              g->base_patch.width, g->base_patch.height, &combined_roi);
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-roi ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }

  if(combined_roi.scale <= 1e-6f
     || roi_out->width <= 0 || roi_out->height <= 0) return FALSE;

  const gboolean same_geometry = (g->process_patch_valid && g->process_patch.pixels
                                  && g->process_patch.width == roi_out->width
                                  && g->process_patch.height == roi_out->height
                                  && abs(g->process_combined_roi.x - combined_roi.x) <= 2
                                  && abs(g->process_combined_roi.y - combined_roi.y) <= 2
                                  && g->process_combined_roi.width == combined_roi.width
                                  && g->process_combined_roi.height == combined_roi.height
                                  && fabsf(g->process_combined_roi.scale - combined_roi.scale) <= 1e-2f);
  if(same_geometry)
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=process-patch-hit ms=%.3f total=%.3f",
             (now - t) / 1000.0, (now - t0) / 1000.0);
    return TRUE;
  }

  /* During active drawing, keep serving the current display-sized process tile
   * even if upstream ROI metadata jitters slightly between recomputes.
   * Rebuilding this tile costs hundreds of ms and kills interaction latency. */
  if(g->process_patch_valid && g->process_patch.pixels && (g->painting || _rt_workers_active(g)))
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF,
             "[drawlayer] process step=process-patch-keep-live ms=%.3f total=%.3f painting=%d workers=%d",
             (now - t) / 1000.0, (now - t0) / 1000.0, g->painting ? 1 : 0, _rt_workers_active(g) ? 1 : 0);
    return TRUE;
  }
  if(g->process_patch_valid && g->process_patch.pixels)
  {
    dt_print(DT_DEBUG_PERF,
             "[drawlayer] process step=process-patch-miss old=(x=%d y=%d w=%d h=%d s=%.6f pw=%d ph=%d) "
             "new=(x=%d y=%d w=%d h=%d s=%.6f pw=%d ph=%d)\n",
             g->process_combined_roi.x, g->process_combined_roi.y, g->process_combined_roi.width,
             g->process_combined_roi.height, g->process_combined_roi.scale,
             g->process_patch.width, g->process_patch.height,
             combined_roi.x, combined_roi.y, combined_roi.width, combined_roi.height, combined_roi.scale,
             roi_out->width, roi_out->height);
  }

  if(g->process_patch_valid && g->process_patch.pixels)
  {
    /* Geometry changed: fold incremental process-tile edits back into base
     * only if this transformed tile actually diverged from base content. */
    if(g->process_patch_dirty)
    {
      _flush_process_patch_to_base(self, g);
      _rekey_shared_base_patch(&g->base_patch, self->dev->image_storage.id,
                               (const dt_iop_drawlayer_params_t *)self->params);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=flush-old-process-patch ms=%.3f",
                 (now - t) / 1000.0);
        t = now;
      }
    }
  }

  if(!_ensure_process_patch_buffer(g, roi_out->width, roi_out->height))
    return FALSE;
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=ensure-process-patch-buffer ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }

  g->process_patch.x = 0;
  g->process_patch.y = 0;
  g->process_combined_roi = combined_roi;

  const dt_iop_roi_t source_full_roi = {
    .x = 0,
    .y = 0,
    .width = g->base_patch.width,
    .height = g->base_patch.height,
    .scale = 1.0f,
  };
  _patch_rdlock(&g->base_patch);
  dt_iop_clip_and_zoom(g->process_patch.pixels, g->base_patch.pixels, &g->process_combined_roi, &source_full_roi,
                       g->process_patch.width, g->base_patch.width);
  _patch_rdunlock(&g->base_patch);
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-patch-clipzoom ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }
  g->process_geom_hash = _drawlayer_process_cache_hash(piece, roi_in, roi_out);
  g->process_patch_valid = TRUE;
  g->process_patch_dirty = FALSE;
  g->process_dirty_bounds_valid = FALSE;
  g->process_dirty_x0 = 0;
  g->process_dirty_y0 = 0;
  g->process_dirty_x1 = 0;
  g->process_dirty_y1 = 0;
  _reset_input_path_state(&g->process_path);
  if(g->process_stroke_mask && g->process_stroke_mask_width == g->process_patch.width
     && g->process_stroke_mask_height == g->process_patch.height)
  {
    memset(g->process_stroke_mask, 0,
           (size_t)g->process_stroke_mask_width * g->process_stroke_mask_height * sizeof(float));
  }
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->process_patch.pixels, NULL, -1);
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-patch-finalize ms=%.3f total=%.3f",
             (now - t) / 1000.0, (now - t0) / 1000.0);
  }
  return TRUE;
}

static G_GNUC_UNUSED void _sample_process_patch_bilinear(const drawlayer_patch_t *patch, const float x, const float y,
                                           float out[4])
{
  if(!patch || !patch->pixels || !out || patch->width <= 0 || patch->height <= 0)
  {
    if(out) memset(out, 0, 4 * sizeof(float));
    return;
  }

  const float sx = fminf(fmaxf(x, 0.0f), (float)(patch->width - 1));
  const float sy = fminf(fmaxf(y, 0.0f), (float)(patch->height - 1));
  const int x0 = (int)floorf(sx);
  const int y0 = (int)floorf(sy);
  const int x1 = MIN(x0 + 1, patch->width - 1);
  const int y1 = MIN(y0 + 1, patch->height - 1);
  const float tx = sx - x0;
  const float ty = sy - y0;

  const float *p00 = patch->pixels + 4 * ((size_t)y0 * patch->width + x0);
  const float *p10 = patch->pixels + 4 * ((size_t)y0 * patch->width + x1);
  const float *p01 = patch->pixels + 4 * ((size_t)y1 * patch->width + x0);
  const float *p11 = patch->pixels + 4 * ((size_t)y1 * patch->width + x1);

  for(int c = 0; c < 4; c++)
  {
    const float a = p00[c] + (p10[c] - p00[c]) * tx;
    const float b = p01[c] + (p11[c] - p01[c]) * tx;
    out[c] = a + (b - a) * ty;
  }
}

static void _flush_process_patch_to_base(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g)
{
  const gint64 t0 = g_get_monotonic_time();
  gint64 t = t0;
  /* `process_patch` lives in current process/output coordinates.
   * Before invalidating it (ROI change, module unfocus, explicit save), rescale
   * it back to source/layer coordinates and fold it into the authoritative
   * `base_patch`. */
  if(!g || !g->process_patch_valid || !g->process_patch.pixels || !g->base_patch.pixels) return;
  if(!g->process_patch_dirty) return;
  if(g->process_patch.width <= 0 || g->process_patch.height <= 0) return;
  if(g->base_patch.width <= 0 || g->base_patch.height <= 0) return;
  if(g->process_combined_roi.scale <= 1e-6f) return;

  const float inv_scale = 1.0f / g->process_combined_roi.scale;
  int src_x0 = MAX((int)floorf(g->process_combined_roi.x * inv_scale), 0);
  int src_y0 = MAX((int)floorf(g->process_combined_roi.y * inv_scale), 0);
  int src_x1 = MIN((int)ceilf((g->process_combined_roi.x + g->process_patch.width) * inv_scale),
                   g->base_patch.width);
  int src_y1 = MIN((int)ceilf((g->process_combined_roi.y + g->process_patch.height) * inv_scale),
                   g->base_patch.height);
  if(g->process_dirty_bounds_valid)
  {
    src_x0 = CLAMP(g->process_dirty_x0, 0, g->base_patch.width);
    src_y0 = CLAMP(g->process_dirty_y0, 0, g->base_patch.height);
    src_x1 = CLAMP(g->process_dirty_x1, 0, g->base_patch.width);
    src_y1 = CLAMP(g->process_dirty_y1, 0, g->base_patch.height);
  }
  if(src_x1 <= src_x0 || src_y1 <= src_y0) return;
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=flush-process-bounds ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }

  const int dst_w = src_x1 - src_x0;
  const int dst_h = src_y1 - src_y0;
  if(!_ensure_process_update_patch_buffer(g, dst_w, dst_h) || !g->process_update_patch.pixels)
    return;
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=flush-process-ensure-update-buffer ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }

  const dt_iop_roi_t process_roi = {
    .x = 0,
    .y = 0,
    .width = g->process_patch.width,
    .height = g->process_patch.height,
    .scale = 1.0f,
  };
  const dt_iop_roi_t inverse_roi = {
    .x = (int)lroundf((float)src_x0 - g->process_combined_roi.x * inv_scale),
    .y = (int)lroundf((float)src_y0 - g->process_combined_roi.y * inv_scale),
    .width = dst_w,
    .height = dst_h,
    .scale = inv_scale,
  };
  dt_iop_clip_and_zoom(g->process_update_patch.pixels, g->process_patch.pixels, &inverse_roi, &process_roi,
                       dst_w, g->process_patch.width);
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=flush-process-clipzoom ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }

  _patch_wrlock(&g->base_patch);
  for(int yy = 0; yy < dst_h; yy++)
  {
    memcpy(g->base_patch.pixels + 4 * ((size_t)(src_y0 + yy) * g->base_patch.width + src_x0),
           g->process_update_patch.pixels + 4 * ((size_t)yy * dst_w),
           (size_t)dst_w * 4 * sizeof(float));
  }
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=flush-process-memcpy ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }

  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->base_patch.pixels, NULL, -1);
  _patch_wrunlock(&g->base_patch);
  g->cache_dirty = TRUE;
  if(self && self->params && self->dev)
    _rekey_shared_base_patch(&g->base_patch, self->dev->image_storage.id,
                             (const dt_iop_drawlayer_params_t *)self->params);
  g->process_patch_dirty = FALSE;
  g->process_dirty_bounds_valid = FALSE;
  g->process_dirty_x0 = 0;
  g->process_dirty_y0 = 0;
  g->process_dirty_x1 = 0;
  g->process_dirty_y1 = 0;
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=flush-process-finalize ms=%.3f total=%.3f",
             (now - t) / 1000.0, (now - t0) / 1000.0);
  }
}

static void _copy_input_to_output(const float *input, float *output, const int width, const int height)
{
  if(!input || !output || width <= 0 || height <= 0) return;
  dt_iop_image_copy_by_size(output, input, width, height, 4);
}

static void _blend_layer_over_input(float *output, const float *input, const float *layerbuf, const size_t pixels,
                                    const gboolean use_preview_bg, const float preview_bg)
{
  if(!output || !input || !layerbuf || pixels == 0) return;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) \
  dt_omp_firstprivate(input, output, layerbuf, pixels, use_preview_bg, preview_bg)
#endif
  for(size_t kk = 0; kk < pixels; kk++)
  {
    const float *base = input + 4 * kk;
    const float *layer = layerbuf + 4 * kk;
    float *pixel = output + 4 * kk;
    const float base_r = use_preview_bg ? preview_bg : base[0];
    const float base_g = use_preview_bg ? preview_bg : base[1];
    const float base_b = use_preview_bg ? preview_bg : base[2];
    const float base_a = use_preview_bg ? 1.0f : base[3];
    const float src_alpha = _clamp01(layer[3]);
    if(src_alpha > 1e-8f)
    {
      const float inv_alpha = 1.0f - src_alpha;
      pixel[0] = layer[0] + base_r * inv_alpha;
      pixel[1] = layer[1] + base_g * inv_alpha;
      pixel[2] = layer[2] + base_b * inv_alpha;
      pixel[3] = src_alpha + base_a * inv_alpha;
    }
    else
    {
      pixel[0] = base_r;
      pixel[1] = base_g;
      pixel[2] = base_b;
      pixel[3] = base_a;
    }
  }
}

#ifdef HAVE_OPENCL
static int _blend_layer_over_input_cl(const int devid, const int kernel_premult_over, cl_mem dev_out, cl_mem dev_in,
                                      drawlayer_process_scratch_t *scratch, const float *layer_pixels,
                                      dt_pixel_cache_entry_t *source_entry,
                                      const int source_w, const int source_h,
                                      const dt_iop_roi_t *const target_roi,
                                      const dt_iop_roi_t *const source_roi,
                                      const gboolean direct_copy,
                                      const gboolean use_preview_bg, const float preview_bg)
{
  if(devid < 0 || !dev_out || !dev_in || !scratch || !layer_pixels || source_w <= 0 || source_h <= 0
     || !target_roi || target_roi->width <= 0 || target_roi->height <= 0)
    return FALSE;

  cl_mem dev_source_rgba = NULL;
  cl_mem dev_layer_rgba = NULL;
  cl_mem dev_background = NULL;
  gboolean source_is_pinned = FALSE;
  int err = CL_SUCCESS;
  int result = FALSE;
  if(kernel_premult_over < 0) return FALSE;

  /* GUI process tiles are externally allocated and updated concurrently by the
   * realtime backend worker. For those buffers (`source_entry == NULL`), use an
   * explicit host->device copy snapshot per process call so OpenCL always sees
   * a coherent full layer, instead of reading a host-pinned buffer while the
   * worker may still be writing into it. Cached/headless sources keep the
   * pinned fast path. */
  if(source_entry)
  {
    dev_source_rgba = dt_dev_pixelpipe_cache_get_pinned_image(darktable.pixelpipe_cache, (void *)layer_pixels,
                                                              source_entry, devid, source_w, source_h,
                                                              4 * sizeof(float),
                                                              CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, NULL, NULL);
    source_is_pinned = TRUE;
  }
  else
  {
    dev_source_rgba = dt_opencl_copy_host_to_device(devid, (void *)layer_pixels, source_w, source_h, 4 * sizeof(float));
    source_is_pinned = FALSE;
  }
  if(!dev_source_rgba) goto cleanup;

  dev_layer_rgba = dev_source_rgba;
  if(!direct_copy)
  {
    dev_layer_rgba = dt_opencl_alloc_device(devid, target_roi->width, target_roi->height, 4 * sizeof(float));
    if(!dev_layer_rgba) goto cleanup;

    err = dt_iop_clip_and_zoom_roi_cl(devid, dev_layer_rgba, dev_source_rgba, target_roi, source_roi);
    if(err != CL_SUCCESS) goto cleanup;
  }

  if(use_preview_bg)
  {
    const size_t out_pixels = (size_t)target_roi->width * target_roi->height;
    float *background = _ensure_process_scratch_buffer(&scratch->cl_background_rgba,
                                                       &scratch->cl_background_rgba_pixels, out_pixels);
    if(!background) goto cleanup;
    for(size_t kk = 0; kk < out_pixels; kk++)
    {
      float *pixel = background + 4 * kk;
      pixel[0] = preview_bg;
      pixel[1] = preview_bg;
      pixel[2] = preview_bg;
      pixel[3] = 1.0f;
    }
    dev_background = dt_dev_pixelpipe_cache_get_pinned_image(darktable.pixelpipe_cache, background, NULL, devid,
                                                             target_roi->width, target_roi->height,
                                                             4 * sizeof(float),
                                                             CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, NULL, NULL);
    if(!dev_background) goto cleanup;
  }
  else
    dev_background = dev_in;

  const int offs[2] = { 0, 0 };
  const size_t sizes[] = { ROUNDUPDWD(target_roi->width, devid), ROUNDUPDHT(target_roi->height, devid), 1 };

  err = dt_opencl_set_kernel_arg(devid, kernel_premult_over, 0, sizeof(cl_mem), &dev_background);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 1, sizeof(cl_mem), &dev_layer_rgba);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 2, sizeof(cl_mem), &dev_out);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 3, sizeof(int), &target_roi->width);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 4, sizeof(int), &target_roi->height);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 5, sizeof(offs), offs);
  if(err != CL_SUCCESS) goto cleanup;

  err = dt_opencl_enqueue_kernel_2d(devid, kernel_premult_over, sizes);
  if(err != CL_SUCCESS) goto cleanup;

  result = TRUE;

cleanup:
  if(use_preview_bg) dt_dev_pixelpipe_cache_put_pinned_image(darktable.pixelpipe_cache, scratch->cl_background_rgba,
                                                             NULL, -1, (void **)&dev_background);
  if(dev_layer_rgba && dev_layer_rgba != dev_source_rgba) dt_opencl_release_mem_object(dev_layer_rgba);
  if(source_is_pinned)
    dt_dev_pixelpipe_cache_put_pinned_image(darktable.pixelpipe_cache, (void *)layer_pixels, source_entry, -1,
                                            (void **)&dev_source_rgba);
  else if(dev_source_rgba)
    dt_opencl_release_mem_object(dev_source_rgba);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[drawlayer] process_cl blend path failed: %d\n", err);

  return result;
}
#endif

/* Sizing step 2 helper: locate the module-input full rectangle inside the current
 * pipeline full canvas.
 *
 * After step 1, the layer is expressed in the "current full pipeline canvas"
 * basis (`pipe->iwidth/iheight`).
 *
 * Upstream geometric modules may already have reduced the *effective* full image
 * seen by this module. That propagated full-input rectangle is `piece->buf_in`:
 * - `buf_in.width/height`: size of the full image region reaching drawlayer,
 * - `buf_in.x/y`: offset of that region in the current full canvas.
 *
 * In most darkroom crop cases the offsets are explicit in `buf_in.x/y`.
 * Thumbnail pipes are a corner case: the image may be uniformly fitted and centered
 * inside the pipeline full canvas, while `buf_in.x/y` remain 0 and only one
 * dimension shrinks. In that specific case, derive the missing origin from the
 * centering margin so we crop the fitted image region rather than the top-left
 * corner of the temporary canvas. */
static void _effective_buf_in_origin(const dt_dev_pixelpipe_iop_t *piece, const int current_full_w,
                                     const int current_full_h, int *x, int *y)
{
  int ox = piece ? piece->buf_in.x : 0;
  int oy = piece ? piece->buf_in.y : 0;

  if(piece)
  {
    if(ox == 0 && piece->buf_in.width < current_full_w && piece->buf_in.height == current_full_h)
      ox = MAX((current_full_w - piece->buf_in.width) / 2, 0);
    if(oy == 0 && piece->buf_in.height < current_full_h && piece->buf_in.width == current_full_w)
      oy = MAX((current_full_h - piece->buf_in.height) / 2, 0);
  }

  if(x) *x = ox;
  if(y) *y = oy;
}

static void _virtual_piece_input_offset(dt_iop_module_t *self, int *x, int *y)
{
  int ox = 0;
  int oy = 0;

  if(self && self->dev && self->dev->virtual_pipe)
  {
    dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->virtual_pipe, self);
    if(piece)
    {
      ox = piece->buf_in.x;
      oy = piece->buf_in.y;
    }
  }

  if(x) *x = ox;
  if(y) *y = oy;
}

static inline void _load_half_pixel_rgba(const uint16_t *src, float rgba[4])
{
  rgba[0] = _half_to_float(src[0]);
  rgba[1] = _half_to_float(src[1]);
  rgba[2] = _half_to_float(src[2]);
  rgba[3] = _half_to_float(src[3]);
}

static void _clear_transparent_argb(unsigned char *buffer, const int stride, const int width, const int height,
                                    const int x0, const int y0, const int x1, const int y1)
{
  if(!buffer || stride <= 0 || width <= 0 || height <= 0) return;

  const int left = CLAMP(x0, 0, width);
  const int top = CLAMP(y0, 0, height);
  const int right = CLAMP(x1, 0, width);
  const int bottom = CLAMP(y1, 0, height);
  if(right <= left || bottom <= top) return;

  for(int y = top; y < bottom; y++)
  {
    unsigned char *row = buffer + (size_t)y * stride + 4 * left;
    memset(row, 0, (size_t)(right - left) * 4);
  }
}

static gboolean _string_field_is_sane(const char *value, const size_t value_size)
{
  if(!value || value_size == 0) return FALSE;
  return memchr(value, '\0', value_size) != NULL;
}

static gboolean _profile_key_is_sane(const char *value)
{
  if(!value || value[0] == '\0') return FALSE;

  int separators = 0;
  for(const unsigned char *c = (const unsigned char *)value; *c; c++)
  {
    if(*c == '|') separators++;
    else if(!g_ascii_isprint(*c)) return FALSE;
  }

  return separators >= 2;
}

static int64_t _sidecar_timestamp_from_path(const char *path)
{
  if(!path || path[0] == '\0' || !g_file_test(path, G_FILE_TEST_EXISTS)) return 0;

  GStatBuf st = { 0 };
  if(g_stat(path, &st) != 0) return 0;
  return (int64_t)st.st_mtime;
}

static void _ensure_gui_conf_defaults(void)
{
  if(!dt_conf_key_exists(DRAWLAYER_CONF_BRUSH_SHAPE)) dt_conf_set_int(DRAWLAYER_CONF_BRUSH_SHAPE, DT_IOP_DRAWLAYER_BRUSH_LINEAR);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_BRUSH_MODE)) dt_conf_set_int(DRAWLAYER_CONF_BRUSH_MODE, DT_IOP_DRAWLAYER_MODE_PAINT);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_COLOR_R)) dt_conf_set_float(DRAWLAYER_CONF_COLOR_R, 1.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_COLOR_G)) dt_conf_set_float(DRAWLAYER_CONF_COLOR_G, 1.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_COLOR_B)) dt_conf_set_float(DRAWLAYER_CONF_COLOR_B, 1.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SOFTNESS)) dt_conf_set_float(DRAWLAYER_CONF_SOFTNESS, 0.5f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_OPACITY)) dt_conf_set_float(DRAWLAYER_CONF_OPACITY, 100.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_FLOW)) dt_conf_set_float(DRAWLAYER_CONF_FLOW, 100.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_BRISTLES)) dt_conf_set_float(DRAWLAYER_CONF_BRISTLES, 0.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SPRINKLES)) dt_conf_set_float(DRAWLAYER_CONF_SPRINKLES, 0.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_BRISTLE_SIZE)) dt_conf_set_float(DRAWLAYER_CONF_BRISTLE_SIZE, 8.0f);
  if(!dt_conf_key_exists(DRAWLAYER_CONF_SPRINKLE_SIZE)) dt_conf_set_float(DRAWLAYER_CONF_SPRINKLE_SIZE, 3.0f);
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

static dt_iop_drawlayer_brush_shape_t _conf_brush_shape(void)
{
  return (dt_iop_drawlayer_brush_shape_t)CLAMP(dt_conf_get_int(DRAWLAYER_CONF_BRUSH_SHAPE),
                                               DT_IOP_DRAWLAYER_BRUSH_LINEAR, DT_IOP_DRAWLAYER_BRUSH_SIGMOIDAL);
}

static dt_iop_drawlayer_brush_mode_t _conf_brush_mode(void)
{
  return (dt_iop_drawlayer_brush_mode_t)CLAMP(dt_conf_get_int(DRAWLAYER_CONF_BRUSH_MODE),
                                              DT_IOP_DRAWLAYER_MODE_PAINT, DT_IOP_DRAWLAYER_MODE_SMUDGE);
}

static float _conf_size(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SIZE), 1.0f, 2048.0f);
}

static float _conf_opacity(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_OPACITY), 0.0f, 100.0f);
}

static float _conf_flow(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_FLOW), 0.0f, 100.0f);
}

static float _conf_bristles(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_BRISTLES), 0.0f, 100.0f);
}

static float _conf_sprinkles(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SPRINKLES), 0.0f, 100.0f);
}

static float _conf_bristle_size(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_BRISTLE_SIZE), 1.0f, 256.0f);
}

static float _conf_sprinkle_size(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SPRINKLE_SIZE), 1.0f, 256.0f);
}

static float _conf_distance(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_DISTANCE), 0.0f, 100.0f);
}

static float _conf_smoothing(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_SMOOTHING), 0.0f, 100.0f);
}

static float _conf_softness(void)
{
  return _clamp01(dt_conf_get_float(DRAWLAYER_CONF_SOFTNESS));
}

static float _conf_hardness(void)
{
  return 1.0f - _conf_softness();
}

static float _conf_hdr_exposure(void)
{
  return CLAMP(dt_conf_get_float(DRAWLAYER_CONF_HDR_EV), 0.0f, 4.0f);
}

static drawlayer_pick_source_t _conf_pick_source(void)
{
  return (drawlayer_pick_source_t)CLAMP(dt_conf_get_int(DRAWLAYER_CONF_PICK_SOURCE),
                                        DRAWLAYER_PICK_SOURCE_INPUT, DRAWLAYER_PICK_SOURCE_OUTPUT);
}

static gboolean _conf_bool(const char *key)
{
  return dt_conf_get_bool(key);
}

static drawlayer_mapping_profile_t _conf_mapping_profile(const char *key)
{
  return (drawlayer_mapping_profile_t)CLAMP(dt_conf_get_int(key), DRAWLAYER_PROFILE_LINEAR, DRAWLAYER_PROFILE_SQRT);
}

static void _conf_display_color(float rgb[3])
{
  rgb[0] = _clamp01(dt_conf_get_float(DRAWLAYER_CONF_COLOR_R));
  rgb[1] = _clamp01(dt_conf_get_float(DRAWLAYER_CONF_COLOR_G));
  rgb[2] = _clamp01(dt_conf_get_float(DRAWLAYER_CONF_COLOR_B));
}

static void _color_history_key(const int index, const char channel, char *key, const size_t key_size)
{
  g_snprintf(key, key_size, DRAWLAYER_CONF_BASE "color_history_%d_%c", index, channel);
}

static void _color_history_valid_key(const int index, char *key, const size_t key_size)
{
  g_snprintf(key, key_size, DRAWLAYER_CONF_BASE "color_history_%d_valid", index);
}

static void _load_color_history(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;

  char key[128] = { 0 };
  for(int i = 0; i < DRAWLAYER_COLOR_HISTORY_COUNT; i++)
  {
    _color_history_valid_key(i, key, sizeof(key));
    g->color_history_valid[i] = dt_conf_key_exists(key) ? dt_conf_get_bool(key) : FALSE;

    _color_history_key(i, 'r', key, sizeof(key));
    g->color_history[i][0] = _clamp01(dt_conf_key_exists(key) ? dt_conf_get_float(key) : 0.0f);
    _color_history_key(i, 'g', key, sizeof(key));
    g->color_history[i][1] = _clamp01(dt_conf_key_exists(key) ? dt_conf_get_float(key) : 0.0f);
    _color_history_key(i, 'b', key, sizeof(key));
    g->color_history[i][2] = _clamp01(dt_conf_key_exists(key) ? dt_conf_get_float(key) : 0.0f);
  }
}

static void _store_color_history(const dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;

  char key[128] = { 0 };
  for(int i = 0; i < DRAWLAYER_COLOR_HISTORY_COUNT; i++)
  {
    _color_history_valid_key(i, key, sizeof(key));
    dt_conf_set_bool(key, g->color_history_valid[i]);

    _color_history_key(i, 'r', key, sizeof(key));
    dt_conf_set_float(key, g->color_history[i][0]);
    _color_history_key(i, 'g', key, sizeof(key));
    dt_conf_set_float(key, g->color_history[i][1]);
    _color_history_key(i, 'b', key, sizeof(key));
    dt_conf_set_float(key, g->color_history[i][2]);
  }
}

static gboolean _display_rgb_equal(const float a[3], const float b[3])
{
  return fabsf(a[0] - b[0]) <= 1e-6f && fabsf(a[1] - b[1]) <= 1e-6f && fabsf(a[2] - b[2]) <= 1e-6f;
}

static void _remember_display_color(dt_iop_module_t *self, const float display_rgb[3])
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !display_rgb) return;

  if(g->color_history_valid[0] && _display_rgb_equal(g->color_history[0], display_rgb)) return;

  for(int i = DRAWLAYER_COLOR_HISTORY_COUNT - 1; i > 0; i--)
  {
    g->color_history_valid[i] = g->color_history_valid[i - 1];
    g->color_history[i][0] = g->color_history[i - 1][0];
    g->color_history[i][1] = g->color_history[i - 1][1];
    g->color_history[i][2] = g->color_history[i - 1][2];
  }

  g->color_history_valid[0] = TRUE;
  g->color_history[0][0] = _clamp01(display_rgb[0]);
  g->color_history[0][1] = _clamp01(display_rgb[1]);
  g->color_history[0][2] = _clamp01(display_rgb[2]);
  _store_color_history(g);
  if(g->color_swatch) gtk_widget_queue_draw(g->color_swatch);
}

static void _apply_display_brush_color(dt_iop_module_t *self, const float display_rgb[3], const gboolean remember)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !g || !display_rgb) return;

  dt_conf_set_float(DRAWLAYER_CONF_COLOR_R, _clamp01(display_rgb[0]));
  dt_conf_set_float(DRAWLAYER_CONF_COLOR_G, _clamp01(display_rgb[1]));
  dt_conf_set_float(DRAWLAYER_CONF_COLOR_B, _clamp01(display_rgb[2]));

  _sync_picker_from_display_rgb(g, display_rgb);
  g->color_surface_dirty = TRUE;
  _clear_cursor_stamp_surface(g);

  if(remember) _remember_display_color(self, display_rgb);

  if(g->color) gtk_widget_queue_draw(g->color);
  if(g->color_swatch) gtk_widget_queue_draw(g->color_swatch);
  dt_control_queue_redraw_center();
}

static void _picker_update_polar_from_uv(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  g->picker_hue = atan2f(g->picker_v, g->picker_u);
  g->picker_chroma = hypotf(g->picker_u, g->picker_v);
}

static void _picker_update_uv_from_polar(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  g->picker_u = g->picker_chroma * cosf(g->picker_hue);
  g->picker_v = g->picker_chroma * sinf(g->picker_hue);
}

static int _picker_project_opponent_to_display_rgb(const float m, const float u, const float v, float display_rgb[3])
{
  const float r = m + u * 0.70710678f + v * 0.40824829f;
  const float g = m - u * 0.70710678f + v * 0.40824829f;
  const float b = m - v * 0.81649658f;

  if(!isfinite(r) || !isfinite(g) || !isfinite(b)) return 1;
  if(r < 0.0f || r > 1.0f || g < 0.0f || g > 1.0f || b < 0.0f || b > 1.0f) return 1;

  display_rgb[0] = r;
  display_rgb[1] = g;
  display_rgb[2] = b;
  return 0;
}

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

  if(!isfinite(limit) || limit < 0.0f) return 0.0f;
  return limit;
}

static void _picker_clamp_state_to_gamut(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;

  g->picker_m = CLAMP(g->picker_m, 0.0f, 1.0f);
  _picker_update_polar_from_uv(g);
  const float max_chroma = _picker_max_chroma_for_m_hue(g->picker_m, g->picker_hue);
  if(g->picker_chroma > max_chroma)
  {
    g->picker_chroma = max_chroma;
    _picker_update_uv_from_polar(g);
  }
}

static int _picker_display_rgb_from_state(const dt_iop_drawlayer_gui_data_t *g, float display_rgb[3])
{
  return _picker_project_opponent_to_display_rgb(g->picker_m, g->picker_u, g->picker_v, display_rgb);
}

static void _sync_picker_from_display_rgb(dt_iop_drawlayer_gui_data_t *g, const float display_rgb[3])
{
  if(!g) return;

  g->picker_m = (display_rgb[0] + display_rgb[1] + display_rgb[2]) / 3.0f;
  g->picker_u = (display_rgb[0] - display_rgb[1]) * 0.70710678f;
  g->picker_v = (display_rgb[0] + display_rgb[1] - 2.0f * display_rgb[2]) * 0.40824829f;
  _picker_clamp_state_to_gamut(g);
}

static void _sync_color_picker_from_conf(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  float display_rgb[3] = { 0.0f };
  _conf_display_color(display_rgb);

  _sync_picker_from_display_rgb(g, display_rgb);
  g->color_surface_dirty = TRUE;
  if(g->color_swatch) gtk_widget_queue_draw(g->color_swatch);
}

static void _color_picker_apply_state(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  _picker_clamp_state_to_gamut(g);

  float display_rgb[3] = { 0.0f };
  if(_picker_display_rgb_from_state(g, display_rgb)) return;
  _apply_display_brush_color(self, display_rgb, FALSE);
}

static void _clear_color_picker_surface(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(g->color_surface)
  {
    cairo_surface_destroy(g->color_surface);
    g->color_surface = NULL;
  }
  g->color_surface_width = 0;
  g->color_surface_height = 0;
  g->color_surface_ppd = 0.0;
  g->color_surface_dirty = TRUE;
}

/* Implemented in paint.c (included later). The cursor stamp preview reuses the
 * exact same fall-off model as the real brush path. */
static inline float _brush_profile(const drawlayer_dab_t *dab, const float norm2);

static void _clear_cursor_stamp_surface(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(g->cursor_surface)
  {
    cairo_surface_destroy(g->cursor_surface);
    g->cursor_surface = NULL;
  }
  g->cursor_surface_size = 0;
  g->cursor_surface_ppd = 0.0;
  g->cursor_radius = 0.0f;
  g->cursor_opacity = 0.0f;
  g->cursor_hardness = 0.0f;
  g->cursor_shape = -1;
  g->cursor_color[0] = g->cursor_color[1] = g->cursor_color[2] = -1.0f;
}

static void _ensure_cursor_stamp_surface(dt_iop_module_t *self, const float widget_radius)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || widget_radius <= 0.0f) return;

  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  float display_rgb[3] = { 0.0f };
  _conf_display_color(display_rgb);
  const float opacity = _clamp01(_conf_opacity() / 100.0f);
  const float hardness = _clamp01(_conf_hardness());
  const int shape = _conf_brush_shape();
  const int size_px = MAX(2, (int)ceil((2.0f * widget_radius + 2.0f) * ppd));

  const gboolean needs_rebuild
      = !g->cursor_surface || g->cursor_surface_size != size_px || fabs(g->cursor_surface_ppd - ppd) > 1e-9
        || fabsf(g->cursor_radius - widget_radius) > 1e-3f || fabsf(g->cursor_opacity - opacity) > 1e-6f
        || fabsf(g->cursor_hardness - hardness) > 1e-6f || g->cursor_shape != shape
        || fabsf(g->cursor_color[0] - display_rgb[0]) > 1e-6f || fabsf(g->cursor_color[1] - display_rgb[1]) > 1e-6f
        || fabsf(g->cursor_color[2] - display_rgb[2]) > 1e-6f;
  if(!needs_rebuild) return;

  _clear_cursor_stamp_surface(g);
  g->cursor_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size_px, size_px);
  if(cairo_surface_status(g->cursor_surface) != CAIRO_STATUS_SUCCESS)
  {
    _clear_cursor_stamp_surface(g);
    return;
  }
  cairo_surface_set_device_scale(g->cursor_surface, ppd, ppd);

  unsigned char *data = cairo_image_surface_get_data(g->cursor_surface);
  const int stride = cairo_image_surface_get_stride(g->cursor_surface);
  memset(data, 0, (size_t)stride * size_px);

  drawlayer_dab_t dab = {
    .radius = fmaxf(widget_radius, 0.5f),
    .shape = shape,
    .softness = hardness,
  };
  const float half = 0.5f * ((float)size_px / (float)ppd);
  for(int py = 0; py < size_px; py++)
  {
    for(int px = 0; px < size_px; px++)
    {
      const float dx = ((float)px + 0.5f) / (float)ppd - half;
      const float dy = ((float)py + 0.5f) / (float)ppd - half;
      const float norm2 = (dx * dx + dy * dy) / fmaxf(dab.radius * dab.radius, 1e-6f);
      const float alpha = opacity * _brush_profile(&dab, norm2);
      if(alpha <= 0.0f) continue;

      unsigned char *pixel = data + (size_t)py * stride + 4 * px;
      pixel[0] = (unsigned char)CLAMP(roundf(255.0f * _clamp01(display_rgb[2] * alpha)), 0, 255);
      pixel[1] = (unsigned char)CLAMP(roundf(255.0f * _clamp01(display_rgb[1] * alpha)), 0, 255);
      pixel[2] = (unsigned char)CLAMP(roundf(255.0f * _clamp01(display_rgb[0] * alpha)), 0, 255);
      pixel[3] = (unsigned char)CLAMP(roundf(255.0f * _clamp01(alpha)), 0, 255);
    }
  }
  cairo_surface_mark_dirty(g->cursor_surface);

  g->cursor_surface_size = size_px;
  g->cursor_surface_ppd = ppd;
  g->cursor_radius = widget_radius;
  g->cursor_opacity = opacity;
  g->cursor_hardness = hardness;
  g->cursor_shape = shape;
  g->cursor_color[0] = display_rgb[0];
  g->cursor_color[1] = display_rgb[1];
  g->cursor_color[2] = display_rgb[2];
}

static void _set_drawlayer_os_cursor_hidden(const gboolean hidden)
{
  if(!darktable.gui || !darktable.gui->ui) return;

  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  GtkWidget *main = dt_ui_main_window(darktable.gui->ui);
  GdkDisplay *display = gdk_display_get_default();
  if(!display) return;

  const dt_cursor_t cursor_id = hidden ? GDK_BLANK_CURSOR : GDK_LEFT_PTR;
  GdkCursor *cursor = gdk_cursor_new_for_display(display, cursor_id);
  if(!cursor) return;

  if(center && gtk_widget_get_window(center))
    gdk_window_set_cursor(gtk_widget_get_window(center), cursor);
  if(main && gtk_widget_get_window(main))
    gdk_window_set_cursor(gtk_widget_get_window(main), cursor);

  dt_control_set_cursor(cursor_id);
  g_object_unref(cursor);
}

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
  if(uv_x) *uv_x = margin;
  if(uv_y) *uv_y = margin + 0.5f * (usable_h - size);
  if(uv_size) *uv_size = size;
  if(plane_x) *plane_x = margin + size + gap;
  if(plane_y) *plane_y = margin + 0.5f * (usable_h - size);
  if(plane_w) *plane_w = size;
  if(plane_h) *plane_h = size;
}

static float _color_picker_hit_margin(void)
{
  const float ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0f;
  return DT_PIXEL_APPLY_DPI(12.0f) * ppd;
}

static gboolean _rect_contains_with_margin(const float x, const float y, const float rx, const float ry,
                                           const float rw, const float rh, const float margin)
{
  return x >= rx - margin && x <= rx + rw + margin && y >= ry - margin && y <= ry + rh + margin;
}

static gboolean _color_picker_set_from_position(dt_iop_module_t *self, const float x, const float y)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->color) return FALSE;

  float uv_x = 0.0f, uv_y = 0.0f, uv_size = 0.0f;
  float plane_x = 0.0f, plane_y = 0.0f, plane_w = 0.0f, plane_h = 0.0f;
  _color_picker_geometry(g->color, &uv_x, &uv_y, &uv_size, &plane_x, &plane_y, &plane_w, &plane_h);
  const float hit_margin = (g->picker_drag_mode == DRAWLAYER_COLOR_DRAG_NONE) ? 0.0f : _color_picker_hit_margin();
  const gboolean in_uv = _rect_contains_with_margin(x, y, uv_x, uv_y, uv_size, uv_size,
                                                    (g->picker_drag_mode == DRAWLAYER_COLOR_DRAG_DISC) ? hit_margin : 0.0f);
  const gboolean in_plane = _rect_contains_with_margin(x, y, plane_x, plane_y, plane_w, plane_h,
                                                       (g->picker_drag_mode == DRAWLAYER_COLOR_DRAG_PLANE) ? hit_margin : 0.0f);

  if(in_uv)
  {
    const float tx = 2.0f * _clamp01((x - uv_x) / fmaxf(uv_size, 1.0f)) - 1.0f;
    const float ty = 1.0f - 2.0f * _clamp01((y - uv_y) / fmaxf(uv_size, 1.0f));
    g->picker_u = tx * DRAWLAYER_COLOR_PICKER_U_MAX;
    g->picker_v = ty * DRAWLAYER_COLOR_PICKER_V_MAX;
    _picker_clamp_state_to_gamut(g);
    g->picker_drag_mode = DRAWLAYER_COLOR_DRAG_DISC;
    _color_picker_apply_state(self);
  }
  else if(in_plane)
  {
    const float tx = _clamp01((x - plane_x) / fmaxf(plane_w, 1.0f));
    const float ty = _clamp01((y - plane_y) / fmaxf(plane_h, 1.0f));
    const float new_m = 1.0f - ty;
    const float new_chroma = tx * DRAWLAYER_COLOR_PICKER_C_MAX;
    const float new_u = new_chroma * cosf(g->picker_hue);
    const float new_v = new_chroma * sinf(g->picker_hue);
    float rgb[3] = { 0.0f };
    if(_picker_project_opponent_to_display_rgb(new_m, new_u, new_v, rgb)) return FALSE;

    g->picker_m = new_m;
    g->picker_chroma = new_chroma;
    g->picker_u = new_u;
    g->picker_v = new_v;
    g->picker_drag_mode = DRAWLAYER_COLOR_DRAG_PLANE;
    _color_picker_apply_state(self);
  }
  else
  {
    return FALSE;
  }
  return TRUE;
}

static gboolean _color_picker_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return FALSE;

  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  if(width <= 0 || height <= 0) return FALSE;
  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  const int width_px = MAX(1, (int)ceil(width * ppd));
  const int height_px = MAX(1, (int)ceil(height * ppd));
  const gboolean size_changed = !g->color_surface || g->color_surface_width != width_px
                                || g->color_surface_height != height_px
                                || fabs(g->color_surface_ppd - ppd) > 1e-9;

  if(size_changed)
  {
    _clear_color_picker_surface(g);
    g->color_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_px, height_px);
    if(cairo_surface_status(g->color_surface) != CAIRO_STATUS_SUCCESS)
    {
      _clear_color_picker_surface(g);
      return FALSE;
    }
    cairo_surface_set_device_scale(g->color_surface, ppd, ppd);
    g->color_surface_width = width_px;
    g->color_surface_height = height_px;
    g->color_surface_ppd = ppd;
    g->color_surface_dirty = TRUE;
  }

  float uv_x = 0.0f, uv_y = 0.0f, uv_size = 0.0f;
  float plane_x = 0.0f, plane_y = 0.0f, plane_w = 0.0f, plane_h = 0.0f;
  _color_picker_geometry(widget, &uv_x, &uv_y, &uv_size, &plane_x, &plane_y, &plane_w, &plane_h);

  if(g->color_surface_dirty)
  {
    unsigned char *data = cairo_image_surface_get_data(g->color_surface);
    const int stride = cairo_image_surface_get_stride(g->color_surface);
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
          const float u = tx * DRAWLAYER_COLOR_PICKER_U_MAX;
          const float v = ty * DRAWLAYER_COLOR_PICKER_V_MAX;
          if(_picker_project_opponent_to_display_rgb(g->picker_m, u, v, rgb)) paint = FALSE;
        }
        else if(fx >= plane_x && fx <= plane_x + plane_w && fy >= plane_y && fy <= plane_y + plane_h)
        {
          const float tx = _clamp01((fx - plane_x) / fmaxf(plane_w, 1.0f));
          const float ty = _clamp01((fy - plane_y) / fmaxf(plane_h, 1.0f));
          const float m = 1.0f - ty;
          const float chroma = tx * DRAWLAYER_COLOR_PICKER_C_MAX;
          const float u = chroma * cosf(g->picker_hue);
          const float v = chroma * sinf(g->picker_hue);
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
    cairo_surface_mark_dirty(g->color_surface);
    g->color_surface_dirty = FALSE;
  }

  cairo_set_source_surface(cr, g->color_surface, 0.0, 0.0);
  cairo_paint(cr);

  /* Hollow markers on the opponent u/v plane and the m/chroma plane. */
  const float uv_mark_x
      = uv_x + 0.5f * ((g->picker_u / fmaxf(DRAWLAYER_COLOR_PICKER_U_MAX, 1e-6f)) + 1.0f) * uv_size;
  const float uv_mark_y
      = uv_y + 0.5f * (1.0f - (g->picker_v / fmaxf(DRAWLAYER_COLOR_PICKER_V_MAX, 1e-6f))) * uv_size;
  const float plane_mark_x = plane_x + _clamp01(g->picker_chroma / DRAWLAYER_COLOR_PICKER_C_MAX) * plane_w;
  const float plane_mark_y = plane_y + (1.0f - _clamp01(g->picker_m)) * plane_h;
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

static gboolean _color_swatch_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  if(!g || width <= 0 || height <= 0) return FALSE;

  const float cell_w = (float)width / DRAWLAYER_COLOR_HISTORY_COLS;
  const float cell_h = (float)height / DRAWLAYER_COLOR_HISTORY_ROWS;
  const float gap = DT_PIXEL_APPLY_DPI(3.0f);

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.12);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_fill(cr);

  for(int i = 0; i < DRAWLAYER_COLOR_HISTORY_COUNT; i++)
  {
    const int row = i / DRAWLAYER_COLOR_HISTORY_COLS;
    const int col = i % DRAWLAYER_COLOR_HISTORY_COLS;
    const float x = col * cell_w;
    const float y = row * cell_h;

    if(g->color_history_valid[i])
      cairo_set_source_rgb(cr, g->color_history[i][0], g->color_history[i][1], g->color_history[i][2]);
    else
      cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);

    cairo_rectangle(cr, x + gap * 0.5f, y + gap * 0.5f, MAX(1.0f, cell_w - gap), MAX(1.0f, cell_h - gap));
    cairo_fill(cr);
  }
  return FALSE;
}

static gboolean _color_swatch_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !widget || !event || event->button != 1) return FALSE;

  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  if(width <= 0 || height <= 0) return FALSE;

  const int col = CLAMP((int)floor(event->x / ((float)width / DRAWLAYER_COLOR_HISTORY_COLS)), 0,
                        DRAWLAYER_COLOR_HISTORY_COLS - 1);
  const int row = CLAMP((int)floor(event->y / ((float)height / DRAWLAYER_COLOR_HISTORY_ROWS)), 0,
                        DRAWLAYER_COLOR_HISTORY_ROWS - 1);
  const int index = row * DRAWLAYER_COLOR_HISTORY_COLS + col;
  if(index < 0 || index >= DRAWLAYER_COLOR_HISTORY_COUNT || !g->color_history_valid[index]) return FALSE;

  _apply_display_brush_color(self, g->color_history[index], FALSE);
  return TRUE;
}

static gboolean _working_rgb_to_display_rgb(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                            const float working_rgb[3], float display_rgb[3])
{
  if(!working_rgb || !display_rgb) return FALSE;

  display_rgb[0] = _clamp01(working_rgb[0]);
  display_rgb[1] = _clamp01(working_rgb[1]);
  display_rgb[2] = _clamp01(working_rgb[2]);

  dt_dev_pixelpipe_t *pipe = piece ? piece->pipe : (self && self->dev ? self->dev->pipe : NULL);
  if(!self || !pipe) return TRUE;

  const dt_iop_order_iccprofile_info_t *const work_profile = self->dev
                                                                 ? dt_ioppr_get_iop_work_profile_info(self, self->dev->iop)
                                                                 : NULL;
  const dt_iop_order_iccprofile_info_t *const display_profile = dt_ioppr_get_pipe_output_profile_info(pipe);
  if(!work_profile || !display_profile) return TRUE;

  float in[4] = { working_rgb[0], working_rgb[1], working_rgb[2], 0.0f };
  float out[4] = { working_rgb[0], working_rgb[1], working_rgb[2], 0.0f };
  dt_ioppr_transform_image_colorspace_rgb(in, out, 1, 1, work_profile, display_profile,
                                          "drawlayer picked color");
  display_rgb[0] = _clamp01(out[0]);
  display_rgb[1] = _clamp01(out[1]);
  display_rgb[2] = _clamp01(out[2]);
  return TRUE;
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  (void)picker;
  if(!self || darktable.gui->reset) return;
  const drawlayer_pick_source_t source = _conf_pick_source();
  const float *picked = (source == DRAWLAYER_PICK_SOURCE_OUTPUT) ? self->picked_output_color : self->picked_color;
  const float *picked_min = (source == DRAWLAYER_PICK_SOURCE_OUTPUT) ? self->picked_output_color_min : self->picked_color_min;
  const float *picked_max = (source == DRAWLAYER_PICK_SOURCE_OUTPUT) ? self->picked_output_color_max : self->picked_color_max;
  if(picked_max[0] < picked_min[0]) return;

  float display_rgb[3] = { 0.0f };
  if(!_working_rgb_to_display_rgb(self, piece, picked, display_rgb)) return;
  _apply_display_brush_color(self, display_rgb, TRUE);
}

static gboolean _color_picker_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!event || event->button != 1) return FALSE;
  (void)widget;
  return _color_picker_set_from_position(self, event->x, event->y);
}

static gboolean _color_picker_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  (void)widget;
  (void)event;
  if(g)
  {
    g->picker_drag_mode = DRAWLAYER_COLOR_DRAG_NONE;
    float display_rgb[3] = { 0.0f };
    if(!_picker_display_rgb_from_state(g, display_rgb)) _remember_display_color(self, display_rgb);
  }
  return FALSE;
}

static gboolean _color_picker_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  (void)widget;
  if(!g || !event || g->picker_drag_mode == DRAWLAYER_COLOR_DRAG_NONE) return FALSE;
  return _color_picker_set_from_position(self, event->x, event->y);
}

static void _get_brush_colors(dt_iop_module_t *self, float display_rgb[3], float pipeline_rgb[3])
{
  float raw_display_rgb[3] = { 0.0f };
  _conf_display_color(raw_display_rgb);
  pipeline_rgb[0] = raw_display_rgb[0];
  pipeline_rgb[1] = raw_display_rgb[1];
  pipeline_rgb[2] = raw_display_rgb[2];

  // The GUI overlay is painted through cairo as a display-referred feedback surface.
  display_rgb[0] = _clamp01(raw_display_rgb[0]);
  display_rgb[1] = _clamp01(raw_display_rgb[1]);
  display_rgb[2] = _clamp01(raw_display_rgb[2]);

  if(self && self->dev && self->dev->pipe)
  {
    const dt_iop_order_iccprofile_info_t *const display_profile = dt_ioppr_get_pipe_output_profile_info(self->dev->pipe);
    const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
    if(display_profile && work_profile)
    {
      float in[4] = { raw_display_rgb[0], raw_display_rgb[1], raw_display_rgb[2], 0.0f };
      float out[4] = { raw_display_rgb[0], raw_display_rgb[1], raw_display_rgb[2], 0.0f };
      dt_ioppr_transform_image_colorspace_rgb(in, out, 1, 1, display_profile, work_profile, "drawlayer brush color");
      pipeline_rgb[0] = out[0];
      pipeline_rgb[1] = out[1];
      pipeline_rgb[2] = out[2];
    }
  }

  const float gain = exp2f(_conf_hdr_exposure());
  pipeline_rgb[0] *= gain;
  pipeline_rgb[1] *= gain;
  pipeline_rgb[2] *= gain;
}

static void _sanitize_params(dt_iop_module_t *self, dt_iop_drawlayer_params_t *params)
{
  if(!params) return;

  if(!_string_field_is_sane(params->layer_name, sizeof(params->layer_name)))
    memset(params->layer_name, 0, sizeof(params->layer_name));
  else
    params->layer_name[sizeof(params->layer_name) - 1] = '\0';

  if(!_string_field_is_sane(params->work_profile, sizeof(params->work_profile)))
    memset(params->work_profile, 0, sizeof(params->work_profile));
  else
    params->work_profile[sizeof(params->work_profile) - 1] = '\0';

  if(params->layer_order < -1) params->layer_order = -1;
  if(params->work_profile[0] != '\0' && !_profile_key_is_sane(params->work_profile))
    memset(params->work_profile, 0, sizeof(params->work_profile));

  // Freshly enabled or migrated instances without a concrete sidecar page should adopt the current profile.
  if(params->layer_order < 0 && params->stroke_commit_hash == 0u)
    memset(params->work_profile, 0, sizeof(params->work_profile));

  _ensure_layer_name(self, params);

  if(params->work_profile[0] == '\0' && self && self->dev)
  {
    char current_profile[DRAWLAYER_PROFILE_SIZE] = { 0 };
    if(_get_current_work_profile_key(self, self->dev->iop, self->dev->pipe, current_profile, sizeof(current_profile)))
      g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
  }
}

static gboolean _widget_to_layer_coords(dt_iop_module_t *self, const double wx, const double wy, float *lx, float *ly)
{
  if(!self || !self->dev || !self->dev->virtual_pipe) return FALSE;

  float pt[2] = { (float)wx, (float)wy };
  dt_dev_coordinates_widget_to_image_norm(self->dev, pt, 1);
  dt_dev_coordinates_image_norm_to_image_abs(self->dev, pt, 1);

  if(!dt_dev_distort_backtransform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                        DT_DEV_TRANSFORM_DIR_FORW_EXCL, pt, 1))
    return FALSE;

  int offset_x = 0;
  int offset_y = 0;
  _virtual_piece_input_offset(self, &offset_x, &offset_y);
  pt[0] += offset_x;
  pt[1] += offset_y;

  *lx = pt[0];
  *ly = pt[1];
  return TRUE;
}

/* Sidecar TIFF and layer-name handling live in their own implementation include. */
#include "layerio.c"

/* Brush/path/paint implementation lives in a separate implementation include. */
#include "paint.c"

/* Realtime worker/queue implementation lives in its own implementation include. */
#include "realtime.c"

static void _clear_patch(drawlayer_patch_t *patch)
{
  /* Release a patch payload and zero its geometry so callers can safely test
   * `patch->pixels` as the ownership flag. This is module-level cache/GUI
   * housekeeping, not brush logic, so keep it in drawlayer.c. */
  if(!patch) return;
  if(patch->cache_entry)
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, patch->cache_hash, FALSE, patch->cache_entry);
  else if(patch->external_alloc && patch->pixels)
  {
    void *buffer = patch->pixels;
    dt_pixelpipe_cache_free_align_cache(darktable.pixelpipe_cache, &buffer, "drawlayer patch");
  }
  else
    g_free(patch->pixels);
  memset(patch, 0, sizeof(*patch));
}

static void _clear_live_surface(dt_iop_drawlayer_gui_data_t *g)
{
  /* Destroy the transient GUI overlay buffers.
   *
   * This is separate from the full-resolution layer cache: `live_surface` is
   * just the on-screen feedback canvas for the current visible ROI. */
  if(!g) return;
  if(g->live_surface)
  {
    cairo_surface_destroy(g->live_surface);
    g->live_surface = NULL;
  }
  _free_tracked_temp_buffer((void **)&g->live_argb, "drawlayer preview surface");
  _free_tracked_temp_buffer((void **)&g->preview_stroke_mask, "drawlayer preview stroke mask");
  g->preview_stroke_mask_width = 0;
  g->preview_stroke_mask_height = 0;
  g->live_stride = 0;
  g->live_surface_dirty = FALSE;
  g->live_surface_damage_valid = FALSE;
  g->stroke_damage_valid = FALSE;
  g->preview_drop_pending = FALSE;
}

static void _invalidate_process_patch(dt_iop_drawlayer_gui_data_t *g)
{
  /* Mark the transformed process-tile cache as stale while keeping any
   * allocated storage around for reuse. Geometry changes or out-of-band layer
   * edits use this path; in-stroke backend dabs update the transformed tile
   * incrementally and therefore do not need to invalidate it. */
  if(!g) return;
  g->process_patch_valid = FALSE;
  g->process_patch_dirty = FALSE;
  g->process_dirty_bounds_valid = FALSE;
  g->process_dirty_x0 = 0;
  g->process_dirty_y0 = 0;
  g->process_dirty_x1 = 0;
  g->process_dirty_y1 = 0;
  g->process_geom_hash = 0;
  memset(&g->process_combined_roi, 0, sizeof(g->process_combined_roi));
  _reset_input_path_state(&g->process_path);
  if(g->process_stroke_mask && g->process_stroke_mask_width > 0 && g->process_stroke_mask_height > 0)
    memset(g->process_stroke_mask, 0,
           (size_t)g->process_stroke_mask_width * g->process_stroke_mask_height * sizeof(float));
}

static gboolean _ensure_process_patch_buffer(dt_iop_drawlayer_gui_data_t *g, const int width, const int height)
{
  /* Allocate the transformed process tile lazily and resize it only when the
   * pipeline tile geometry changes. This cache is persistent across recomputes
   * precisely so `process()` can avoid repeated full-tile interpolation. */
  if(!g || width <= 0 || height <= 0) return FALSE;

  const gboolean size_changed = (g->process_patch.width != width || g->process_patch.height != height
                                 || !g->process_patch.pixels);
  if(size_changed)
  {
    _clear_patch(&g->process_patch);
    g->process_patch.width = width;
    g->process_patch.height = height;
    g->process_patch.x = 0;
    g->process_patch.y = 0;
    g->process_patch.pixels = _alloc_tracked_temp_buffer((gsize)width * height * 4 * sizeof(float),
                                                         "drawlayer process tile");
    g->process_patch.external_alloc = TRUE;
    if(!g->process_patch.pixels)
    {
      _invalidate_process_patch(g);
      return FALSE;
    }
  }

  const size_t mask_count = (size_t)width * height;
  if(size_changed || g->process_stroke_mask_width != width || g->process_stroke_mask_height != height
     || !g->process_stroke_mask)
  {
    _free_tracked_temp_buffer((void **)&g->process_stroke_mask, "drawlayer process stroke mask");
    g->process_stroke_mask = _alloc_tracked_temp_buffer(mask_count * sizeof(float),
                                                        "drawlayer process stroke mask");
    if(!g->process_stroke_mask)
    {
      g->process_stroke_mask_width = 0;
      g->process_stroke_mask_height = 0;
      _invalidate_process_patch(g);
      return FALSE;
    }
    g->process_stroke_mask_width = width;
    g->process_stroke_mask_height = height;
  }

  return TRUE;
}

static G_GNUC_UNUSED gboolean _ensure_process_update_patch_buffer(dt_iop_drawlayer_gui_data_t *g, const int width, const int height)
{
  /* The incremental process-tile refresh path resamples only the damaged
   * destination rectangle. `dt_iop_clip_and_zoom()` expects a tightly packed
   * output buffer whose stride matches `roi_out->width`, so we keep a separate
   * reusable scratch patch for those sub-rectangle updates and memcpy it back
   * into the persistent `process_patch`. */
  if(!g || width <= 0 || height <= 0) return FALSE;

  const gboolean size_changed = (g->process_update_patch.width != width || g->process_update_patch.height != height
                                 || !g->process_update_patch.pixels);
  if(!size_changed) return TRUE;

  _clear_patch(&g->process_update_patch);
  g->process_update_patch.width = width;
  g->process_update_patch.height = height;
  g->process_update_patch.x = 0;
  g->process_update_patch.y = 0;
  g->process_update_patch.pixels = _alloc_tracked_temp_buffer((gsize)width * height * 4 * sizeof(float),
                                                              "drawlayer process update tile");
  g->process_update_patch.external_alloc = TRUE;
  return g->process_update_patch.pixels != NULL;
}

static gboolean G_GNUC_UNUSED _refresh_process_patch_from_base_damage(dt_iop_drawlayer_gui_data_t *g,
                                                                      const int src_x0, const int src_y0,
                                                                      const int src_x1, const int src_y1)
{
  /* `process_patch` now stores a direct 1:1 crop of `base_patch` for the
   * visible area. Updating it after a backend dab therefore only requires
   * copying the damaged overlap rectangle, with no resampling at all. */
  if(!g || !g->process_patch_valid || !g->process_patch.pixels || !g->base_patch.pixels) return FALSE;
  if(src_x1 <= src_x0 || src_y1 <= src_y0) return FALSE;
  const int overlap_x0 = MAX(src_x0, g->process_patch.x);
  const int overlap_y0 = MAX(src_y0, g->process_patch.y);
  const int overlap_x1 = MIN(src_x1, g->process_patch.x + g->process_patch.width);
  const int overlap_y1 = MIN(src_y1, g->process_patch.y + g->process_patch.height);
  if(overlap_x1 <= overlap_x0 || overlap_y1 <= overlap_y0)
  {
    return TRUE;
  }

  const int dst_x0 = overlap_x0 - g->process_patch.x;
  const int dst_y0 = overlap_y0 - g->process_patch.y;
  const int copy_w = overlap_x1 - overlap_x0;
  const int copy_h = overlap_y1 - overlap_y0;
  _patch_rdlock(&g->base_patch);
  for(int yy = 0; yy < copy_h; yy++)
  {
    memcpy(g->process_patch.pixels + 4 * ((size_t)(dst_y0 + yy) * g->process_patch.width + dst_x0),
           g->base_patch.pixels + 4 * ((size_t)(overlap_y0 + yy) * g->base_patch.width + overlap_x0),
           (size_t)copy_w * 4 * sizeof(float));
  }
  _patch_rdunlock(&g->base_patch);

  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->process_patch.pixels, NULL, -1);
  return TRUE;
}

static gboolean _layer_cache_matches(const dt_iop_drawlayer_gui_data_t *g, const int32_t imgid, const int raw_width,
                                     const int raw_height, const char *layer_name, const int layer_order)
{
  /* Cache identity belongs to the module orchestration layer because it
   * combines sidecar identity with GUI/module state (active image, selected
   * layer, in-memory patch ownership). The low-level TIFF primitives stay in
   * layerio.c. */
  if(!g || !g->cache_valid || !g->base_patch.pixels) return FALSE;
  if(g->cache_imgid != imgid || g->cache_raw_width != raw_width || g->cache_raw_height != raw_height) return FALSE;

  /* Once a layer is cached in memory, `process()` should treat that payload as
   * authoritative until we explicitly reload or flush it. The page index in the
   * params is only a sidecar lookup hint and may legitimately drift after sidecar
   * rewrites or page reordering. Layer names are unique and stable by design, so
   * prefer them for cache identity. Falling back to the page index here causes
   * `process()` to re-open the TIFF and re-scan directories even though the
   * correct pixels are already in memory. */
  const char *target_name = layer_name ? layer_name : "";
  if(target_name[0] != '\0' || g->cache_layer_name[0] != '\0')
    return !g_strcmp0(g->cache_layer_name, target_name);

  if(layer_order >= 0 && g->cache_layer_order >= 0) return g->cache_layer_order == layer_order;
  return TRUE;
}

static gboolean _ensure_layer_cache(dt_iop_module_t *self)
{
  /* Make sure `base_patch` mirrors the currently selected sidecar layer.
   * The low-level TIFF read/write primitives live in layerio.c, while this
   * function stays here because it orchestrates cache lifetime, widget state,
   * prompting, and history/UI side effects around those I/O operations. */
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g || !self->dev) return FALSE;

  _sanitize_params(self, params);
  const int raw_width = self->dev->roi.raw_width;
  const int raw_height = self->dev->roi.raw_height;
  const int32_t imgid = self->dev->image_storage.id;
  if(imgid <= 0 || raw_width <= 0 || raw_height <= 0) return FALSE;
  const gboolean bootstrap = (params->sidecar_timestamp == 0 && params->layer_order < 0);

  GString *errors = g_string_new(NULL);

  char current_profile[DRAWLAYER_PROFILE_SIZE] = { 0 };
  const gboolean have_current_profile = _get_current_work_profile_key(self, self->dev->iop, self->dev->pipe,
                                                                      current_profile, sizeof(current_profile));
  if(bootstrap && have_current_profile)
    g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
  else if(!have_current_profile)
    _layerio_append_error(errors, _("failed to resolve drawlayer working profile"));
  else if(params->work_profile[0] == '\0')
    g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
  else if(g_strcmp0(params->work_profile, current_profile))
    _layerio_append_error(errors, _("drawlayer working profile mismatch"));

  if(_layer_cache_matches(g, imgid, raw_width, raw_height, params->layer_name, params->layer_order))
  {
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return TRUE;
  }

  if(!_flush_layer_cache(self))
  {
    _layerio_append_error(errors, _("failed to write drawing layer sidecar"));
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return FALSE;
  }
  _invalidate_undo_redo(self);
  /* We are about to replace/rebind `g->base_patch`; drop all explicit extra
   * refs from the previous entry first so counters never leak across entries. */
  _release_all_base_patch_extra_refs(g);

  int created = 0;
  if(!_acquire_shared_base_patch(&g->base_patch, imgid, params, raw_width, raw_height,
                                 "drawlayer sidecar cache", &created))
  {
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    g->cache_valid = FALSE;
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return FALSE;
  }
  if(created)
  {
    _patch_wrlock(&g->base_patch);
    _clear_transparent_float(g->base_patch.pixels, (size_t)raw_width * raw_height);
    _patch_wrunlock(&g->base_patch);
  }

  gboolean ok = TRUE;
  gboolean cache_loaded = FALSE;
  gboolean file_exists = FALSE;
  char path[PATH_MAX] = { 0 };

  if(!created)
  {
    cache_loaded = TRUE;
  }
  else if(!_layerio_sidecar_path(imgid, path, sizeof(path)))
  {
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    g->cache_valid = FALSE;
    _layerio_append_error(errors, _("failed to resolve drawlayer sidecar path"));
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return TRUE;
  }

  if(created) file_exists = g_file_test(path, G_FILE_TEST_EXISTS);

  if(created && bootstrap)
  {
    if(!have_current_profile)
    {
      ok = FALSE;
    }
    else
    {
      char unique_name[DRAWLAYER_NAME_SIZE] = { 0 };
      _layerio_make_unique_name(self, path, params->layer_name, unique_name, sizeof(unique_name));
      g_strlcpy(params->layer_name, unique_name, sizeof(params->layer_name));

      int final_order = -1;
      _patch_rdlock(&g->base_patch);
      const gboolean stored = _layerio_store_layer(path, params->layer_name, -1, params->work_profile,
                                                   &g->base_patch, raw_width, raw_height, FALSE, &final_order);
      _patch_rdunlock(&g->base_patch);
      if(!stored)
      {
        _layerio_append_error(errors, _("failed to initialize drawing layer sidecar"));
        ok = FALSE;
      }
      else
      {
        params->layer_order = final_order;
        params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
        _touch_layer_cache_epoch(g);
        _invalidate_process_patch(g);
        cache_loaded = TRUE;
      }
    }
  }
  else if(created && !file_exists)
  {
    _layerio_append_error(errors, _("drawlayer sidecar TIFF is missing"));
  }
  else if(created)
  {
    drawlayer_dir_info_t info;
    if(!_layerio_find_layer(path, params->layer_name, -1, &info))
    {
      int missing_action = 0;
      if(self->dev && self->dev->gui_module == self)
        missing_action = _offer_missing_layer_recreation(self, params->layer_name);

      if(missing_action == 2)
      {
        if(!have_current_profile)
        {
          _layerio_append_error(errors, _("failed to resolve drawlayer working profile"));
          ok = FALSE;
        }
        else
        {
          g_string_truncate(errors, 0);
          g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
          int final_order = -1;
          _patch_rdlock(&g->base_patch);
          const gboolean stored = _layerio_store_layer(path, params->layer_name, -1, params->work_profile,
                                                       &g->base_patch, raw_width, raw_height, FALSE,
                                                       &final_order);
          _patch_rdunlock(&g->base_patch);
          if(!stored)
          {
            _layerio_append_error(errors, _("failed to initialize drawing layer sidecar"));
            ok = FALSE;
          }
          else
          {
            if(g)
            {
              g->missing_layer_prompted = FALSE;
              g->missing_layer_prompt_name[0] = '\0';
            }
            params->layer_order = final_order;
            params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
            _touch_layer_cache_epoch(g);
            _invalidate_process_patch(g);
            cache_loaded = TRUE;
          }
        }
      }
      else if(missing_action == 0)
      {
        _layerio_append_error(errors, _("drawlayer layer not found in sidecar TIFF"));
      }
    }
    else
    {
      if(g)
      {
        g->missing_layer_prompted = FALSE;
        g->missing_layer_prompt_name[0] = '\0';
      }
      params->layer_order = info.index;
      if(info.work_profile[0] != '\0' && params->work_profile[0] == '\0')
        g_strlcpy(params->work_profile, info.work_profile, sizeof(params->work_profile));

      if(have_current_profile && info.work_profile[0] != '\0' && g_strcmp0(info.work_profile, current_profile))
        _layerio_append_error(errors, _("drawlayer sidecar profile mismatch"));

      _patch_wrlock(&g->base_patch);
      const gboolean loaded = _layerio_load_layer(path, params->layer_name, info.index, raw_width, raw_height,
                                                  &g->base_patch);
      _patch_wrunlock(&g->base_patch);
      if(!loaded)
      {
        _layerio_append_error(errors, _("failed to read drawing layer sidecar"));
        ok = FALSE;
      }
      else
      {
        params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
        _touch_layer_cache_epoch(g);
        _invalidate_process_patch(g);
        cache_loaded = TRUE;
      }
    }
  }

  if(cache_loaded)
  {
    g->cache_valid = TRUE;
    g->cache_dirty = FALSE;
    g->cache_imgid = imgid;
    g->cache_raw_width = raw_width;
    g->cache_raw_height = raw_height;
    g_strlcpy(g->cache_layer_name, params->layer_name, sizeof(g->cache_layer_name));
    g->cache_layer_order = params->layer_order;

    if(g->layer_name && !g->updating && g_strcmp0(gtk_entry_get_text(g->layer_name), params->layer_name))
    {
      g->updating = TRUE;
      gtk_entry_set_text(g->layer_name, params->layer_name);
      g->updating = FALSE;
    }
    if(g->layer_select) _populate_layer_list(self);
    if(created) _retain_base_patch_loaded_ref(g);
  }
  else
  {
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    g->cache_valid = FALSE;
    g->cache_dirty = FALSE;
  }

  _sync_save_button(self);

  _layerio_log_errors(errors);
  g_string_free(errors, TRUE);
  return ok || !cache_loaded;
}

static gboolean _flush_layer_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || !g->cache_valid || !g->cache_dirty || !g->base_patch.pixels) return TRUE;
  if(!_layer_name_non_empty(g->cache_layer_name)) return FALSE;

  /* If the visible transformed tile has been updated incrementally for the
   * current ROI, fold it back into the authoritative raw-sized cache before the
   * sidecar write. That keeps the final flushed layer consistent with what the
   * user actually saw, even if the transformed tile was carrying the most recent
   * stroke-stateful updates. */
  _flush_process_patch_to_base(self, g);

  char path[PATH_MAX] = { 0 };
  const int32_t flush_imgid = (g->cache_imgid > 0) ? g->cache_imgid : self->dev->image_storage.id;
  if(flush_imgid <= 0) return TRUE;
  if(!_layerio_sidecar_path(flush_imgid, path, sizeof(path))) return FALSE;

  int final_order = g->cache_layer_order;
  const dt_iop_drawlayer_params_t *params = (const dt_iop_drawlayer_params_t *)self->params;
  const char *work_profile = params ? params->work_profile : "";
  _patch_rdlock(&g->base_patch);
  const gboolean ok = _layerio_store_layer(path, g->cache_layer_name, g->cache_layer_order, work_profile,
                                           &g->base_patch, g->cache_raw_width, g->cache_raw_height, FALSE,
                                           &final_order);
  _patch_rdunlock(&g->base_patch);
  if(!ok) return FALSE;

  g->cache_layer_order = final_order;
  g->cache_dirty = FALSE;

  dt_iop_drawlayer_params_t *mutable_params = (dt_iop_drawlayer_params_t *)self->params;
  if(mutable_params)
  {
    if(!g_strcmp0(mutable_params->layer_name, g->cache_layer_name)) mutable_params->layer_order = final_order;
    mutable_params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
    _rekey_shared_base_patch(&g->base_patch, flush_imgid, mutable_params);
  }
  _release_all_base_patch_extra_refs(g);
  return TRUE;
}

static gboolean _ensure_widget_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return FALSE;

  drawlayer_patch_t visible = { 0 };
  float view_x0 = 0.0f, view_y0 = 0.0f, view_x1 = 0.0f, view_y1 = 0.0f;
  if(!_compute_view_patch(self, 0.0f, &visible, &view_x0, &view_y0, &view_x1, &view_y1)) return FALSE;

  float wx0 = 0.0f, wy0 = 0.0f, wx1 = 0.0f, wy1 = 0.0f;
  if(!_layer_bounds_to_widget_bounds(self, view_x0, view_y0, view_x1, view_y1, &wx0, &wy0, &wx1, &wy1)) return FALSE;

  const gboolean same_view = (g->live_patch.width == visible.width && g->live_patch.height == visible.height
                              && g->live_patch.x == visible.x && g->live_patch.y == visible.y
                              && fabsf(g->live_view_x0 - view_x0) <= 1e-6f && fabsf(g->live_view_y0 - view_y0) <= 1e-6f
                              && fabsf(g->live_view_x1 - view_x1) <= 1e-6f && fabsf(g->live_view_y1 - view_y1) <= 1e-6f
                              && fabsf(g->preview_x0 - wx0) <= 1e-6f && fabsf(g->preview_y0 - wy0) <= 1e-6f
                              && fabsf(g->preview_x1 - wx1) <= 1e-6f && fabsf(g->preview_y1 - wy1) <= 1e-6f
                              && g->live_argb && g->live_surface);

  if(same_view)
  {
    g->last_view_x = self->dev->roi.x;
    g->last_view_y = self->dev->roi.y;
    g->last_view_scale = self->dev->roi.scaling;
    return TRUE;
  }

  _clear_live_surface(g);
  g->live_patch = visible;
  g->live_patch.pixels = NULL;
  g->live_view_x0 = view_x0;
  g->live_view_y0 = view_y0;
  g->live_view_x1 = view_x1;
  g->live_view_y1 = view_y1;
  g->preview_x0 = wx0;
  g->preview_y0 = wy0;
  g->preview_x1 = wx1;
  g->preview_y1 = wy1;

  const int width = MAX(1, (int)ceilf(fmaxf(wx1 - wx0, 1.0f)));
  const int height = MAX(1, (int)ceilf(fmaxf(wy1 - wy0, 1.0f)));
  g->live_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
  g->live_argb = _alloc_tracked_temp_buffer((gsize)g->live_stride * height, "drawlayer preview surface");
  if(!g->live_argb)
  {
    g->live_stride = 0;
    return FALSE;
  }
  const size_t mask_count = (size_t)width * height;
  g->preview_stroke_mask = _alloc_tracked_temp_buffer(mask_count * sizeof(float), "drawlayer preview stroke mask");
  if(!g->preview_stroke_mask)
  {
    _clear_live_surface(g);
    return FALSE;
  }
  g->preview_stroke_mask_width = width;
  g->preview_stroke_mask_height = height;
  memset(g->preview_stroke_mask, 0, mask_count * sizeof(float));
  _clear_transparent_argb(g->live_argb, g->live_stride, width, height, 0, 0, width, height);

  g->live_surface = cairo_image_surface_create_for_data(g->live_argb, CAIRO_FORMAT_ARGB32, width, height, g->live_stride);
  if(cairo_surface_status(g->live_surface) != CAIRO_STATUS_SUCCESS)
  {
    _clear_live_surface(g);
    return FALSE;
  }

  _paint_clear_stroke_state(&g->stroke);
  g->live_dirty = FALSE;
  g->preview_drop_pending = FALSE;
  if(g->preview_dabs) g_array_set_size(g->preview_dabs, 0);
  g->last_view_x = self->dev->roi.x;
  g->last_view_y = self->dev->roi.y;
  g->last_view_scale = self->dev->roi.scaling;
  return TRUE;
}

static void _clear_widget_stroke_locked(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->stroke.widget_damage_valid || !g->live_argb || !g->live_surface || g->live_stride <= 0) return;

  const int width = cairo_image_surface_get_width(g->live_surface);
  const int height = cairo_image_surface_get_height(g->live_surface);
  const int x0 = CLAMP(g->stroke.widget_x0, 0, width);
  const int y0 = CLAMP(g->stroke.widget_y0, 0, height);
  const int x1 = CLAMP(g->stroke.widget_x1, 0, width);
  const int y1 = CLAMP(g->stroke.widget_y1, 0, height);
  if(x1 <= x0 || y1 <= y0) return;

  _clear_transparent_argb(g->live_argb, g->live_stride, width, height, x0, y0, x1, y1);
  if(g->preview_stroke_mask && g->preview_stroke_mask_width == width && g->preview_stroke_mask_height == height)
  {
    for(int y = y0; y < y1; y++)
      memset(g->preview_stroke_mask + (size_t)y * width + x0, 0, (size_t)(x1 - x0) * sizeof(float));
  }

  cairo_surface_mark_dirty_rectangle(g->live_surface, x0, y0, x1 - x0, y1 - y0);
}

static float _current_live_padding(dt_iop_module_t *self)
{
  drawlayer_dab_t dab = {
    .radius = fmaxf(_conf_size(), 0.5f),
    .softness = _conf_hardness(),
    .shape = _conf_brush_shape(),
  };
  return ceilf(dab.radius + 1.0f);
}

static gboolean _compute_view_patch(dt_iop_module_t *self, const float padding, drawlayer_patch_t *patch,
                                    float *x0, float *y0, float *x1, float *y1)
{
  if(!self || !self->dev || !patch) return FALSE;

  const int raw_width = self->dev->roi.raw_width;
  const int raw_height = self->dev->roi.raw_height;
  if(raw_width <= 0 || raw_height <= 0) return FALSE;

  const float widget_w = (float)self->dev->roi.orig_width;
  const float widget_h = (float)self->dev->roi.orig_height;
  const float preview_w = self->dev->roi.preview_width;
  const float preview_h = self->dev->roi.preview_height;
  if(widget_w <= 0.0f || widget_h <= 0.0f || preview_w <= 0.0f || preview_h <= 0.0f) return FALSE;

  const float zoom_scale = dt_dev_get_overlay_scale(self->dev);
  const float border = (float)self->dev->roi.border_size;
  const float roi_w = fminf(widget_w, preview_w * zoom_scale);
  const float roi_h = fminf(widget_h, preview_h * zoom_scale);
  const float rec_x = fmaxf(border, 0.5f * (widget_w - roi_w));
  const float rec_y = fmaxf(border, 0.5f * (widget_h - roi_h));
  const float rec_w = fminf(widget_w - 2.0f * border, roi_w);
  const float rec_h = fminf(widget_h - 2.0f * border, roi_h);
  if(rec_w <= 0.0f || rec_h <= 0.0f) return FALSE;

  float pts[8] = { rec_x, rec_y, rec_x + rec_w, rec_y, rec_x, rec_y + rec_h, rec_x + rec_w, rec_y + rec_h };
  for(int i = 0; i < 4; i++)
  {
    if(!_widget_to_layer_coords(self, pts[2 * i], pts[2 * i + 1], &pts[2 * i], &pts[2 * i + 1])) return FALSE;
  }

  float min_x = pts[0];
  float max_x = pts[0];
  float min_y = pts[1];
  float max_y = pts[1];
  for(int i = 1; i < 4; i++)
  {
    min_x = fminf(min_x, pts[2 * i]);
    max_x = fmaxf(max_x, pts[2 * i]);
    min_y = fminf(min_y, pts[2 * i + 1]);
    max_y = fmaxf(max_y, pts[2 * i + 1]);
  }

  if(x0) *x0 = min_x;
  if(y0) *y0 = min_y;
  if(x1) *x1 = max_x;
  if(y1) *y1 = max_y;

  patch->x = MAX(0, (int)floorf(min_x - padding));
  patch->y = MAX(0, (int)floorf(min_y - padding));
  const int right = MIN(raw_width, (int)ceilf(max_x + padding));
  const int bottom = MIN(raw_height, (int)ceilf(max_y + padding));
  patch->width = MAX(0, right - patch->x);
  patch->height = MAX(0, bottom - patch->y);
  patch->pixels = NULL;
  return patch->width > 0 && patch->height > 0;
}

static gboolean _sync_temp_buffers(dt_iop_module_t *self, const gboolean flush_pending, const gboolean record_history)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return FALSE;

  if(flush_pending && g->stroke.active && !_commit_dabs(self, record_history)) return FALSE;
  _pause_worker(self);
  if(!_ensure_layer_cache(self))
  {
    _resume_worker(self);
    return FALSE;
  }
  if(!_ensure_widget_cache(self))
  {
    _resume_worker(self);
    return FALSE;
  }

  g->live_mode = _conf_brush_mode();
  g->live_padding = _current_live_padding(self);
  _resume_worker(self);
  return TRUE;
}

static float _widget_brush_radius(dt_iop_module_t *self, const drawlayer_dab_t *dab, const float fallback)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !dab) return fallback;

  float pts[6] = {
    dab->x, dab->y,
    dab->x + dab->radius, dab->y,
    dab->x, dab->y + dab->radius,
  };

  if(!dt_dev_distort_transform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 3))
    return fallback;

  dt_dev_coordinates_image_abs_to_image_norm(self->dev, pts, 3);
  dt_dev_coordinates_image_norm_to_widget(self->dev, pts, 3);

  const float rx = hypotf(pts[2] - pts[0], pts[3] - pts[1]);
  const float ry = hypotf(pts[4] - pts[0], pts[5] - pts[1]);
  const float radius = 0.5f * (rx + ry);
  return fmaxf(0.5f, isfinite(radius) ? radius : fallback);
}

static gboolean _layer_to_widget_coords(dt_iop_module_t *self, const float x, const float y, float *wx, float *wy)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !wx || !wy) return FALSE;

  int offset_x = 0;
  int offset_y = 0;
  _virtual_piece_input_offset(self, &offset_x, &offset_y);

  float pt[2] = { x - offset_x, y - offset_y };
  if(!dt_dev_distort_transform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pt, 1))
    return FALSE;

  dt_dev_coordinates_image_abs_to_image_norm(self->dev, pt, 1);
  dt_dev_coordinates_image_norm_to_widget(self->dev, pt, 1);
  *wx = pt[0];
  *wy = pt[1];
  return TRUE;
}

static void _touch_stroke_commit_hash(dt_iop_drawlayer_params_t *params, const int dab_count)
{
  if(!params) return;

  const uint64_t seed[2] = { (uint64_t)(uint32_t)dab_count, (uint64_t)g_get_monotonic_time() };

  uint64_t hash = params->stroke_commit_hash ? params->stroke_commit_hash : 5381u;
  hash = dt_hash(hash, (const char *)seed, sizeof(seed));

  /* Keep the serialized field non-zero so "uninitialized" remains distinguishable
   * from "updated at least once" in legacy parameter blobs. */
  params->stroke_commit_hash = (uint32_t)(hash ? hash : 1u);
}

static gboolean _checkpoint_stroke_history(dt_iop_module_t *self)
{
  /* Periodic in-stroke checkpoints are intentionally lighter than the final
   * stroke commit:
   * - keep the workers running,
   * - keep the live stroke buffers intact,
   * - request a main-pipe refresh without appending history states.
   *
   * The actual history entry is still committed once on stroke end.
   */
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!g || !params || !self->dev) return FALSE;
  if(!g->painting || g->finish_commit_pending) return FALSE;

  const int sample_count = (int)g->stroke_sample_count;
  if(sample_count <= 0) return FALSE;

  /* Avoid hammering the GUI thread with history commits while display pipes are
   * already busy processing a previous checkpoint. Queuing another
   * refresh request at that point tends to stall interactions and
   * does not improve visual latency. */
  if((self->dev->preview_pipe && self->dev->preview_pipe->processing)
     || (self->dev->pipe && self->dev->pipe->processing))
    return FALSE;

  /* Deliberately do not call `_wait_worker_idle()` or flush
   * `process_patch -> base_patch` here.
   *
   * During realtime drawing, backend dabs already update `process_patch`
   * directly and `process()/process_cl()` blend that tile directly. Forcing a
   * full upsample into `base_patch` every 250 ms turned out to dominate the
   * OpenCL path cost.
   *
   * Keep heavy rescale commits for stroke end / explicit save / ROI
   * invalidation only. Checkpoints here are render nudges only. */
  _touch_stroke_commit_hash(params, sample_count);
  dt_dev_pixelpipe_refresh_main(self->dev, FALSE);
  return TRUE;
}

static void _sync_undo_redo_buttons(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  if(g->undo_button) gtk_widget_set_sensitive(g->undo_button, g->undo_available && !g->painting);
  if(g->redo_button) gtk_widget_set_sensitive(g->redo_button, g->redo_available && !g->painting);
}

static void _sync_save_button(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->save_layer) return;

  /* The sidecar save action should stay available whenever we have an
   * authoritative layer cache in memory. Unlike undo/redo, it is useful while
   * the module is focused because it is the explicit persistence escape hatch
   * for users who do not want to rely on focus-loss or shutdown hooks. */
  gtk_widget_set_sensitive(g->save_layer, g->cache_valid && g->base_patch.pixels);
}

static void _refresh_layer_widgets(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!g || !params) return;

  if(g->layer_name)
  {
    g->updating = TRUE;
    gtk_entry_set_text(g->layer_name, params->layer_name);
    g->updating = FALSE;
  }

  if(g->layer_select) _populate_layer_list(self);
  if(g->create_background) gtk_widget_set_sensitive(g->create_background, !g->background_job_running);
  _sync_save_button(self);
}

static void _invalidate_undo_redo(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  _clear_patch(&g->undo_patch);
  _free_tracked_temp_buffer((void **)&g->stroke_mask, "drawlayer stroke mask");
  g->stroke_mask_width = 0;
  g->stroke_mask_height = 0;
  g->undo_available = FALSE;
  g->redo_available = FALSE;
  _sync_undo_redo_buttons(self);
}

static gboolean _prepare_undo_snapshot(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->cache_valid || !g->base_patch.pixels) return FALSE;

  const size_t count = (size_t)g->base_patch.width * g->base_patch.height * 4;
  if(count == 0) return FALSE;

  if(g->undo_patch.width != g->base_patch.width || g->undo_patch.height != g->base_patch.height || !g->undo_patch.pixels)
  {
    const uint64_t undo_hash = g->base_patch.cache_hash
                                   ? dt_hash(g->base_patch.cache_hash, "undo", strlen("undo"))
                                   : _drawlayer_sidecar_cache_hash(g->cache_layer_name, g->cache_layer_name, "undo");
    if(!_alloc_cache_patch(&g->undo_patch, undo_hash, (size_t)g->base_patch.width * g->base_patch.height,
                           g->base_patch.width, g->base_patch.height, "drawlayer undo cache", NULL))
    {
      _clear_patch(&g->undo_patch);
      g->undo_available = FALSE;
      g->redo_available = FALSE;
      return FALSE;
    }
    g->undo_patch.x = g->base_patch.x;
    g->undo_patch.y = g->base_patch.y;
  }
  else
  {
    g->undo_patch.x = g->base_patch.x;
    g->undo_patch.y = g->base_patch.y;
  }

  _patch_rdlock(&g->base_patch);
  memcpy(g->undo_patch.pixels, g->base_patch.pixels, count * sizeof(float));
  _patch_rdunlock(&g->base_patch);

  const size_t mask_count = (size_t)g->base_patch.width * g->base_patch.height;
  if(g->stroke_mask_width != g->base_patch.width || g->stroke_mask_height != g->base_patch.height || !g->stroke_mask)
  {
    _free_tracked_temp_buffer((void **)&g->stroke_mask, "drawlayer stroke mask");
    g->stroke_mask = _alloc_tracked_temp_buffer(mask_count * sizeof(float), "drawlayer stroke mask");
    if(!g->stroke_mask)
    {
      g->stroke_mask_width = 0;
      g->stroke_mask_height = 0;
      g->undo_available = TRUE;
      g->redo_available = FALSE;
      return TRUE;
    }
    g->stroke_mask_width = g->base_patch.width;
    g->stroke_mask_height = g->base_patch.height;
  }
  memset(g->stroke_mask, 0, mask_count * sizeof(float));

  g->undo_available = TRUE;
  g->redo_available = FALSE;

  _rt_schedule_async_undo_ui(self);
  return TRUE;
}

static gboolean _swap_undo_redo(dt_iop_module_t *self, const gboolean undo)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  if((undo && !g->undo_available) || (!undo && !g->redo_available)) return FALSE;
  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_sync_temp_buffers(self, FALSE, FALSE)) return FALSE;
  if(!g->base_patch.pixels || !g->undo_patch.pixels) return FALSE;

  _release_all_base_patch_extra_refs(g);
  drawlayer_patch_t tmp = g->base_patch;
  g->base_patch = g->undo_patch;
  g->undo_patch = tmp;
  g->cache_valid = TRUE;
  g->cache_dirty = TRUE;
  _touch_layer_cache_epoch(g);
  _invalidate_process_patch(g);

  if(undo)
  {
    g->undo_available = FALSE;
    g->redo_available = TRUE;
  }
  else
  {
    g->undo_available = TRUE;
    g->redo_available = FALSE;
  }

  _touch_stroke_commit_hash(params, 0);
  _sync_undo_redo_buttons(self);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  dt_control_queue_redraw_center();
  return TRUE;
}

static void _sync_preview_bg_buttons(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  g->updating = TRUE;
  if(GTK_IS_TOGGLE_BUTTON(g->preview_bg_image))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preview_bg_image), g->preview_bg_mode == DRAWLAYER_PREVIEW_BG_IMAGE);
  if(GTK_IS_TOGGLE_BUTTON(g->preview_bg_white))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preview_bg_white), g->preview_bg_mode == DRAWLAYER_PREVIEW_BG_WHITE);
  if(GTK_IS_TOGGLE_BUTTON(g->preview_bg_grey))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preview_bg_grey), g->preview_bg_mode == DRAWLAYER_PREVIEW_BG_GREY);
  if(GTK_IS_TOGGLE_BUTTON(g->preview_bg_black))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preview_bg_black), g->preview_bg_mode == DRAWLAYER_PREVIEW_BG_BLACK);
  g->updating = FALSE;
}

static gboolean _commit_dabs(dt_iop_module_t *self, const gboolean record_history)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g || !self->dev) return TRUE;

  _cancel_async_commit(self);
  _cancel_async_history_tick(self);

  /* Commit ordering is strict:
   * - the preview queue and backend queue workers must be idle,
   * - the layer cache must already contain the stroke,
   * - only then do we mutate params/history so the pipeline invalidation sees a coherent state. */
  _wait_worker_idle(self);

  int sample_count = 0;
  gboolean had_stroke = FALSE;
  dt_iop_gui_enter_critical_section(self);
  g->finish_commit_pending = FALSE;
  _rt_clear_backend_status_active(g);
  sample_count = (int)g->stroke_sample_count;
  had_stroke = (sample_count > 0) || g->pending_stroke_applied;
  if(had_stroke)
  {
    if(record_history)
      g->preview_drop_pending = TRUE;
    else
    {
      _clear_widget_stroke_locked(g);
      _paint_clear_stroke_state(&g->stroke);
      g->live_dirty = FALSE;
      g->preview_drop_pending = FALSE;
    }
  }
  if(g->dabs) g_array_set_size(g->dabs, 0);
  if(g->preview_dabs) g_array_set_size(g->preview_dabs, 0);
  if(g->backend_dabs) g_array_set_size(g->backend_dabs, 0);
  if(g->preview_history) g_array_set_size(g->preview_history, 0);
  if(g->backend_history) g_array_set_size(g->backend_history, 0);
  g->stroke_sample_count = 0;
  _paint_reset_stroke_runtime(g);
  g->pending_stroke_applied = FALSE;
  if(g->stroke_mask) memset(g->stroke_mask, 0, (size_t)g->stroke_mask_width * g->stroke_mask_height * sizeof(float));
  if(g->preview_stroke_mask)
    memset(g->preview_stroke_mask, 0,
           (size_t)g->preview_stroke_mask_width * g->preview_stroke_mask_height * sizeof(float));
  dt_iop_gui_leave_critical_section(self);

  if(had_stroke)
  {
    /* Keep stroke commit lightweight for interactive rendering:
     * - do not flush process tile back to base patch here,
     * - do not invalidate the process tile here.
     *
     * Base-patch synchronization is handled at explicit persistence points
     * (save/focus-out/mouse-leave/geometry rebuild) so recomputes can keep
     * blending the already-updated process tile directly. */
    _touch_stroke_commit_hash(params, sample_count);
    _retain_base_patch_stroke_ref(g);
    if(record_history && self->dev)
    {
      dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    }
    dt_control_queue_redraw_center();
  }

  return TRUE;
}

static void _develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  (void)instance;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !self->dev) return;

  /* Keep the preview overlay visible until the UI pipe has produced the committed result.
   * Otherwise the user would see the live stroke disappear before the pipeline catches up. */
  if(g->preview_drop_pending && !g->painting)
  {
    _clear_widget_stroke_locked(g);
    _paint_clear_stroke_state(&g->stroke);
    g->live_dirty = FALSE;
    g->preview_drop_pending = FALSE;
    dt_control_queue_redraw_center();
  }

  const gboolean view_changed = (fabsf(g->last_view_x - self->dev->roi.x) > 1e-6f
                                 || fabsf(g->last_view_y - self->dev->roi.y) > 1e-6f
                                 || fabsf(g->last_view_scale - self->dev->roi.scaling) > 1e-6f);
  const gboolean padding_changed = fabsf(g->live_padding - _current_live_padding(self)) > 1e-6f;
  if(!view_changed && !padding_changed) return;

  const gboolean worker_active = _rt_workers_active(g);

  /* Never flush or rebuild buffers from the pipe-finished callback while a stroke is active
   * or while the worker still has pending stroke work. Doing so races the asynchronous stroke
   * commit path and can block forever waiting for the worker from inside the callback. */
  if(g->painting || g->stroke.active || worker_active) return;

  _cancel_async_resync(self);

  if(!_sync_temp_buffers(self, FALSE, FALSE))
  {
    /* This callback performs a best-effort cache resync after a completed UI-pipe render.
     * A transient failure here does not mean the stroke failed; it usually means the
     * transformed view rectangle was temporarily unavailable during the pipe transition.
     * Consume the current ROI state so we do not spam the same warning after every stroke. */
    g->last_view_x = self->dev->roi.x;
    g->last_view_y = self->dev->roi.y;
    g->last_view_scale = self->dev->roi.scaling;
    g->live_padding = _current_live_padding(self);
  }
  else
    dt_control_queue_redraw_center();
}

static void _sync_mode_sensitive_widgets(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->color || !g->softness) return;

  const gboolean paint_mode = (_conf_brush_mode() == DT_IOP_DRAWLAYER_MODE_PAINT);
  const gboolean show_hardness = (_conf_brush_shape() != DT_IOP_DRAWLAYER_BRUSH_GAUSSIAN);
  gtk_widget_set_visible(GTK_WIDGET(g->color), paint_mode);
  if(g->color_row) gtk_widget_set_visible(g->color_row, paint_mode);
  if(g->color_swatch) gtk_widget_set_visible(g->color_swatch, paint_mode);
  if(g->image_colorpicker) gtk_widget_set_visible(g->image_colorpicker, paint_mode);
  if(g->image_colorpicker_source) gtk_widget_set_visible(g->image_colorpicker_source, paint_mode);
  gtk_widget_set_visible(g->softness, show_hardness);
}

static gboolean _delete_current_layer(dt_iop_module_t *self)
{
  if(!self->dev) return FALSE;

  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _ensure_layer_name(self, params);

  GString *errors = g_string_new(NULL);
  gboolean deleted = FALSE;

  char path[PATH_MAX] = { 0 };
  if(!_layerio_sidecar_path(self->dev->image_storage.id, path, sizeof(path)))
  {
    _layerio_append_error(errors, _("failed to resolve drawlayer sidecar path"));
  }
  else if(!g_file_test(path, G_FILE_TEST_EXISTS))
  {
    _layerio_append_error(errors, _("drawlayer sidecar TIFF is missing"));
  }
  else
  {
    drawlayer_dir_info_t info;
    if(!_layerio_find_layer(path, params->layer_name, -1, &info))
    {
      _layerio_append_error(errors, _("drawlayer layer not found in sidecar TIFF"));
    }
    else if(!_layerio_store_layer(path, params->layer_name, info.index, NULL, NULL,
                                  self->dev->roi.raw_width, self->dev->roi.raw_height, TRUE, NULL))
    {
      _layerio_append_error(errors, _("failed to delete drawing layer from sidecar"));
    }
    else
    {
      deleted = TRUE;
      _default_layer_name(self, params->layer_name, sizeof(params->layer_name));
      params->layer_order = -1;
      params->sidecar_timestamp = 0;
      memset(params->work_profile, 0, sizeof(params->work_profile));
      if(g)
      {
        _release_all_base_patch_extra_refs(g);
        _clear_patch(&g->base_patch);
        g->cache_valid = FALSE;
        g->cache_dirty = FALSE;
        g->cache_layer_name[0] = '\0';
        g->cache_layer_order = -1;
        _clear_widget_stroke_locked(g);
        _paint_clear_stroke_state(&g->stroke);
        g->live_dirty = FALSE;
      }
      _refresh_layer_widgets(self);
    }
  }

  _layerio_log_errors(errors);
  g_string_free(errors, TRUE);
  return deleted;
}

static gboolean _confirm_delete_layer(dt_iop_module_t *self, const gboolean removing_module)
{
  if(!self->dev) return FALSE;

  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _ensure_layer_name(self, params);

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
      GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE,
      "%s",
      removing_module
          ? _("Delete the linked drawing layer from the sidecar TIFF before removing this module instance?")
          : _("Delete the linked drawing layer from the sidecar TIFF?"));
  if(removing_module)
  {
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("Keep layer"), GTK_RESPONSE_NO);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("Delete layer"), GTK_RESPONSE_YES);
  }
  else
  {
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("Delete"), GTK_RESPONSE_ACCEPT);
  }

  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  if(removing_module)
  {
    if(response == GTK_RESPONSE_YES) _delete_current_layer(self);
    /* Module removal must proceed regardless of the answer or any layer-delete failure. */
    return TRUE;
  }

  if(response != GTK_RESPONSE_ACCEPT) return FALSE;
  return _delete_current_layer(self);
}

static void _sync_params_from_gui(dt_iop_module_t *self, const gboolean record_history)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  (void)record_history;
  if(!g || g->updating) return;

  dt_conf_set_int(DRAWLAYER_CONF_BRUSH_SHAPE, dt_bauhaus_combobox_get(g->brush_shape));
  dt_conf_set_int(DRAWLAYER_CONF_BRUSH_MODE, dt_bauhaus_combobox_get(g->brush_mode));
  dt_conf_set_float(DRAWLAYER_CONF_SIZE, dt_bauhaus_slider_get(g->size));
  dt_conf_set_float(DRAWLAYER_CONF_DISTANCE, dt_bauhaus_slider_get(g->distance));
  dt_conf_set_float(DRAWLAYER_CONF_SMOOTHING, dt_bauhaus_slider_get(g->smoothing));
  dt_conf_set_float(DRAWLAYER_CONF_OPACITY, dt_bauhaus_slider_get(g->opacity));
  dt_conf_set_float(DRAWLAYER_CONF_FLOW, dt_bauhaus_slider_get(g->flow));
  dt_conf_set_float(DRAWLAYER_CONF_BRISTLES, dt_bauhaus_slider_get(g->bristles));
  dt_conf_set_float(DRAWLAYER_CONF_BRISTLE_SIZE, dt_bauhaus_slider_get(g->bristle_size));
  dt_conf_set_float(DRAWLAYER_CONF_SPRINKLES, dt_bauhaus_slider_get(g->sprinkles));
  dt_conf_set_float(DRAWLAYER_CONF_SPRINKLE_SIZE, dt_bauhaus_slider_get(g->sprinkle_size));
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

static gboolean _rename_current_layer_from_gui(dt_iop_module_t *self, const char *requested_name)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  char new_name[DRAWLAYER_NAME_SIZE] = { 0 };
  char stripped_requested[DRAWLAYER_NAME_SIZE] = { 0 };
  if(requested_name) g_strlcpy(stripped_requested, requested_name, sizeof(stripped_requested));
  g_strstrip(stripped_requested);
  if(stripped_requested[0] == '\0') return FALSE;
  _sanitize_requested_layer_name(self, requested_name, new_name, sizeof(new_name));
  if(new_name[0] == '\0') return FALSE;

  if(!g_strcmp0(new_name, params->layer_name))
  {
    if(g_strcmp0(gtk_entry_get_text(g->layer_name), new_name))
    {
      g->updating = TRUE;
      gtk_entry_set_text(g->layer_name, new_name);
      g->updating = FALSE;
    }
    return TRUE;
  }

  GString *errors = g_string_new(NULL);
  gboolean renamed = FALSE;

  if(!_commit_dabs(self, FALSE))
    _layerio_append_error(errors, _("failed to commit drawing stroke before renaming"));
  else if(!_flush_layer_cache(self))
    _layerio_append_error(errors, _("failed to write drawing layer sidecar"));
  else
  {
    char path[PATH_MAX] = { 0 };
    if(!_layerio_sidecar_path(self->dev->image_storage.id, path, sizeof(path)))
      _layerio_append_error(errors, _("failed to resolve drawlayer sidecar path"));
    else if(!g_file_test(path, G_FILE_TEST_EXISTS))
      _layerio_append_error(errors, _("drawlayer sidecar TIFF is missing"));
    else
    {
      drawlayer_dir_info_t info;
      if(!_layerio_find_layer(path, params->layer_name, -1, &info))
        _layerio_append_error(errors, _("drawlayer layer not found in sidecar TIFF"));
      else if(_layerio_layer_name_exists(path, new_name, info.index))
        _layerio_append_error(errors, _("drawlayer layer name already exists"));
      else
      {
        int final_order = info.index;
        if(!_layerio_store_layer(path, new_name, info.index, params->work_profile, NULL,
                                 self->dev->roi.raw_width, self->dev->roi.raw_height, FALSE, &final_order))
        {
          _layerio_append_error(errors, _("failed to rename drawing layer in sidecar"));
        }
        else
        {
          g_strlcpy(params->layer_name, new_name, sizeof(params->layer_name));
          params->layer_order = final_order;
          params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
          g_strlcpy(g->cache_layer_name, params->layer_name, sizeof(g->cache_layer_name));
          g->cache_layer_order = params->layer_order;
          renamed = TRUE;
        }
      }
    }
  }

  if(renamed)
  {
    g->missing_layer_prompted = FALSE;
    g->missing_layer_prompt_name[0] = '\0';
    _refresh_layer_widgets(self);
    if(self->dev) dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  }
  else
  {
    _refresh_layer_widgets(self);
  }

  _layerio_log_errors(errors);
  g_string_free(errors, TRUE);
  return renamed;
}

static gboolean _create_new_layer(dt_iop_module_t *self, const char *requested_name)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  char new_name[DRAWLAYER_NAME_SIZE] = { 0 };
  char stripped_requested[DRAWLAYER_NAME_SIZE] = { 0 };
  if(requested_name) g_strlcpy(stripped_requested, requested_name, sizeof(stripped_requested));
  g_strstrip(stripped_requested);
  if(stripped_requested[0] == '\0') return FALSE;
  _sanitize_requested_layer_name(self, requested_name, new_name, sizeof(new_name));
  if(new_name[0] == '\0') return FALSE;

  const dt_iop_drawlayer_params_t previous = *params;

  _cancel_async_resync(self);
  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_flush_layer_cache(self)) return FALSE;

  g_strlcpy(params->layer_name, new_name, sizeof(params->layer_name));
  params->layer_order = -1;
  params->sidecar_timestamp = 0;
  memset(params->work_profile, 0, sizeof(params->work_profile));

  if(!_sync_temp_buffers(self, FALSE, FALSE))
  {
    *params = previous;
    gui_update(self);
    return FALSE;
  }

  g->missing_layer_prompted = FALSE;
  g->missing_layer_prompt_name[0] = '\0';
  _touch_stroke_commit_hash(params, 0);
  if(self->dev)
  {
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    dt_dev_pixelpipe_refresh_all(self->dev, FALSE);
  }
  _refresh_layer_widgets(self);
  gui_update(self);
  dt_control_queue_redraw_center();
  return TRUE;
}

typedef struct drawlayer_tiff_export_params_t
{
  dt_imageio_module_data_t global;
  int bpp;
  int compress;
  int compresslevel;
  int shortfile;
} drawlayer_tiff_export_params_t;

typedef struct drawlayer_background_job_params_t
{
  int32_t imgid;
  int raw_width;
  int raw_height;
  int dst_x;
  int dst_y;
  int insert_after_order;
  char sidecar_path[PATH_MAX];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
  char requested_bg_name[DRAWLAYER_NAME_SIZE];
  char filter[64];
  char initiator_layer_name[DRAWLAYER_NAME_SIZE];
  int initiator_layer_order;
} drawlayer_background_job_params_t;

typedef struct drawlayer_background_job_result_t
{
  gboolean success;
  int32_t imgid;
  int64_t sidecar_timestamp;
  char created_bg_name[DRAWLAYER_NAME_SIZE];
  char initiator_layer_name[DRAWLAYER_NAME_SIZE];
  int initiator_layer_order;
  char message[DT_CONTROL_DESCRIPTION_LEN];
} drawlayer_background_job_result_t;

static void _build_pre_module_filter_string(dt_iop_module_t *self, char *filter, const size_t filter_size)
{
  if(!filter || filter_size == 0)
    return;
  filter[0] = '\0';
  if(!self || !self->dev || !self->dev->pipe) return;

  dt_dev_pixelpipe_iop_t *self_piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->pipe, self);
  const char *prev_op = NULL;
  if(self_piece && self->dev->pipe)
  {
    for(GList *nodes = self->dev->pipe->nodes; nodes; nodes = g_list_next(nodes))
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
      if(piece == self_piece) break;
      if(piece->enabled && piece->module && piece->module->op[0]) prev_op = piece->module->op;
    }
  }
  if(prev_op && prev_op[0]) g_snprintf(filter, filter_size, "pre:%s", prev_op);
}

static gboolean _export_pre_module_fullres_to_tiff(const int32_t imgid, const char *filter, const char *path)
{
  if(imgid <= 0 || !path || path[0] == '\0') return FALSE;

  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name("tiff");
  if(!format) return FALSE;

  dt_imageio_module_data_t *format_params = format->get_params(format);
  if(!format_params) return FALSE;

  format_params->max_width = 0;
  format_params->max_height = 0;
  format_params->width = 0;
  format_params->height = 0;
  format_params->style[0] = '\0';

  if(format->params_size(format) >= sizeof(drawlayer_tiff_export_params_t))
  {
    drawlayer_tiff_export_params_t *const tiff_params = (drawlayer_tiff_export_params_t *)format_params;
    tiff_params->bpp = 32;
    tiff_params->compress = 0;
    tiff_params->compresslevel = 0;
    tiff_params->shortfile = 0;
  }

  const int rc = dt_imageio_export_with_flags(imgid,
                                              path,
                                              format,
                                              format_params,
                                              TRUE,
                                              FALSE,
                                              TRUE,
                                              FALSE,
                                              FALSE,
                                              (filter && filter[0]) ? filter : NULL,
                                              FALSE,
                                              FALSE,
                                              DT_COLORSPACE_NONE,
                                              NULL,
                                              DT_INTENT_PERCEPTUAL,
                                              NULL,
                                              NULL,
                                              0,
                                              0,
                                              NULL);

  format->free_params(format, format_params);
  return rc == 0;
}

static int32_t _background_layer_job_run(dt_job_t *job)
{
  const drawlayer_background_job_params_t *params = (const drawlayer_background_job_params_t *)dt_control_job_get_params(job);
  if(!params) return 0;

  drawlayer_background_job_result_t *result = g_new0(drawlayer_background_job_result_t, 1);
  result->imgid = params->imgid;
  g_strlcpy(result->initiator_layer_name, params->initiator_layer_name, sizeof(result->initiator_layer_name));
  result->initiator_layer_order = params->initiator_layer_order;
  g_strlcpy(result->message, _("failed to create background layer from input"), sizeof(result->message));

  gchar *tmp_path = NULL;
  const int tmp_fd = g_file_open_tmp("ansel-drawlayer-bg-XXXXXX.tiff", &tmp_path, NULL);
  if(tmp_fd >= 0) g_close(tmp_fd, NULL);

  float *export_pixels = NULL;
  int export_w = 0;
  int export_h = 0;
  drawlayer_patch_t bg_patch = { 0 };

  do
  {
    if(tmp_fd < 0 || !tmp_path) break;
    if(!_export_pre_module_fullres_to_tiff(params->imgid, params->filter, tmp_path)) break;
    if(!_layerio_load_flat_rgba(tmp_path, &export_pixels, &export_w, &export_h)) break;
    if(!export_pixels || export_w <= 0 || export_h <= 0) break;

    bg_patch.width = params->raw_width;
    bg_patch.height = params->raw_height;
    bg_patch.x = 0;
    bg_patch.y = 0;
    bg_patch.pixels = _alloc_tracked_temp_buffer((size_t)params->raw_width * params->raw_height * 4 * sizeof(float),
                                                  "drawlayer bg layer");
    if(!bg_patch.pixels) break;
    _clear_transparent_float(bg_patch.pixels, (size_t)params->raw_width * params->raw_height);

    const int clip_x0 = MAX(params->dst_x, 0);
    const int clip_y0 = MAX(params->dst_y, 0);
    const int clip_x1 = MIN(params->dst_x + export_w, params->raw_width);
    const int clip_y1 = MIN(params->dst_y + export_h, params->raw_height);
    if(clip_x1 <= clip_x0 || clip_y1 <= clip_y0) break;

    const int copy_w = clip_x1 - clip_x0;
    const int copy_h = clip_y1 - clip_y0;
    const int src_x0 = clip_x0 - params->dst_x;
    const int src_y0 = clip_y0 - params->dst_y;
    for(int y = 0; y < copy_h; y++)
    {
      const float *src = export_pixels + 4 * ((size_t)(src_y0 + y) * export_w + src_x0);
      float *dst = bg_patch.pixels + 4 * ((size_t)(clip_y0 + y) * params->raw_width + clip_x0);
      memcpy(dst, src, (size_t)copy_w * 4 * sizeof(float));
    }

    char bg_name[DRAWLAYER_NAME_SIZE] = { 0 };
    _layerio_make_unique_name_plain(params->sidecar_path, params->requested_bg_name, bg_name, sizeof(bg_name));
    if(!_layer_name_non_empty(bg_name)) break;

    int final_order = -1;
    if(!_layerio_insert_layer(params->sidecar_path, bg_name, params->insert_after_order, params->work_profile,
                              &bg_patch, params->raw_width, params->raw_height, &final_order))
      break;

    drawlayer_dir_info_t created_info;
    memset(&created_info, 0, sizeof(created_info));
    if(!_layerio_find_layer(params->sidecar_path, bg_name, final_order, &created_info)) break;

    result->success = TRUE;
    result->sidecar_timestamp = _sidecar_timestamp_from_path(params->sidecar_path);
    g_strlcpy(result->created_bg_name, bg_name, sizeof(result->created_bg_name));
    g_snprintf(result->message, sizeof(result->message), _("created background layer `%s'"), bg_name);
  } while(0);

  if(export_pixels) _free_tracked_temp_buffer((void **)&export_pixels, "drawlayer bg export");
  if(bg_patch.pixels) _free_tracked_temp_buffer((void **)&bg_patch.pixels, "drawlayer bg layer");
  if(tmp_path)
  {
    g_unlink(tmp_path);
    g_free(tmp_path);
  }

  g_main_context_invoke(NULL, _background_layer_job_done_idle, result);
  return 0;
}

static gboolean _background_layer_job_done_idle(gpointer user_data)
{
  drawlayer_background_job_result_t *result = (drawlayer_background_job_result_t *)user_data;
  if(!result) return G_SOURCE_REMOVE;

  dt_control_log("%s", result->message[0] ? result->message : _("background layer job finished"));

  gboolean cleared_initiator = FALSE;
  if(darktable.develop && darktable.develop->image_storage.id == result->imgid)
  {
    for(GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
      if(!module || g_strcmp0(module->op, "drawlayer")) continue;

      dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)module->gui_data;
      dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)module->params;
      if(g && params
         && !g_strcmp0(params->layer_name, result->initiator_layer_name)
         && params->layer_order == result->initiator_layer_order)
      {
        g->background_job_running = FALSE;
        if(g->create_background) gtk_widget_set_sensitive(g->create_background, TRUE);
        cleared_initiator = TRUE;
      }

      if(result->success && params)
      {
        params->sidecar_timestamp = result->sidecar_timestamp;
        _refresh_layer_widgets(module);
      }
    }
  }

  if(!cleared_initiator && darktable.develop)
  {
    for(GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
      if(!module || g_strcmp0(module->op, "drawlayer")) continue;
      dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)module->gui_data;
      if(!g || !g->background_job_running) continue;
      g->background_job_running = FALSE;
      if(g->create_background) gtk_widget_set_sensitive(g->create_background, TRUE);
    }
  }

  if(result->success) dt_control_queue_redraw_center();
  g_free(result);
  return G_SOURCE_REMOVE;
}

static gboolean _create_background_layer_from_input(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;
  if(g->background_job_running) return FALSE;
  _ensure_layer_name(self, params);
  if(!_layer_name_non_empty(params->layer_name)) return FALSE;

  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_ensure_layer_cache(self)) return FALSE;
  if(!_flush_layer_cache(self)) return FALSE;

  const int raw_width = self->dev->roi.raw_width;
  const int raw_height = self->dev->roi.raw_height;
  if(raw_width <= 0 || raw_height <= 0) return FALSE;

  char sidecar_path[PATH_MAX] = { 0 };
  if(!_layerio_sidecar_path(self->dev->image_storage.id, sidecar_path, sizeof(sidecar_path))) return FALSE;

  drawlayer_dir_info_t current_info;
  memset(&current_info, 0, sizeof(current_info));
  if(!_layerio_find_layer(sidecar_path, params->layer_name, params->layer_order, &current_info))
    return FALSE;

  int dst_x = 0;
  int dst_y = 0;
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->pipe, self);
  if(piece)
  {
    dst_x = piece->buf_in.x;
    dst_y = piece->buf_in.y;
  }

  drawlayer_background_job_params_t *job_params = g_new0(drawlayer_background_job_params_t, 1);
  job_params->imgid = self->dev->image_storage.id;
  job_params->raw_width = raw_width;
  job_params->raw_height = raw_height;
  job_params->dst_x = dst_x;
  job_params->dst_y = dst_y;
  job_params->insert_after_order = current_info.index;
  g_strlcpy(job_params->sidecar_path, sidecar_path, sizeof(job_params->sidecar_path));
  g_strlcpy(job_params->work_profile, params->work_profile, sizeof(job_params->work_profile));
  g_snprintf(job_params->requested_bg_name, sizeof(job_params->requested_bg_name), "%s-bg", params->layer_name);
  g_strlcpy(job_params->initiator_layer_name, params->layer_name, sizeof(job_params->initiator_layer_name));
  job_params->initiator_layer_order = params->layer_order;
  _build_pre_module_filter_string(self, job_params->filter, sizeof(job_params->filter));

  dt_job_t *job = dt_control_job_create(_background_layer_job_run, "drawlayer create background layer");
  if(!job)
  {
    g_free(job_params);
    return FALSE;
  }

  dt_control_job_set_params(job, job_params, g_free);
  dt_control_job_add_progress(job, _("creating background layer"), TRUE);
  g->background_job_running = TRUE;
  if(g->create_background) gtk_widget_set_sensitive(g->create_background, FALSE);
  if(dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, job) != 0)
  {
    g->background_job_running = FALSE;
    if(g->create_background) gtk_widget_set_sensitive(g->create_background, TRUE);
    return FALSE;
  }
  return TRUE;
}

static int _offer_missing_layer_recreation(dt_iop_module_t *self, const char *missing_name)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !self->dev || !g) return FALSE;
  if(self->dev->gui_module != self) return 0;

  if(g->missing_layer_prompted && !g_strcmp0(g->missing_layer_prompt_name, missing_name ? missing_name : ""))
    return 1;

  g->missing_layer_prompted = TRUE;
  g_strlcpy(g->missing_layer_prompt_name, missing_name ? missing_name : "", sizeof(g->missing_layer_prompt_name));

  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                             GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_WARNING,
                                             GTK_BUTTONS_NONE,
                                             "%s",
                                             _("The linked drawing layer was not found in the sidecar TIFF."));
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                           "%s",
                                           _("Do you want to create a new layer for this module now?"));
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Keep module as no-op"), GTK_RESPONSE_NO);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Create new layer"), GTK_RESPONSE_YES);

  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  if(response == GTK_RESPONSE_YES) return 2;
  return 1;
}

static gboolean _current_layer_missing_in_sidecar(dt_iop_module_t *self)
{
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !params || params->layer_name[0] == '\0') return FALSE;

  char path[PATH_MAX] = { 0 };
  if(!_layerio_sidecar_path(self->dev->image_storage.id, path, sizeof(path))) return FALSE;
  if(!g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  drawlayer_dir_info_t info;
  return !_layerio_find_layer(path, params->layer_name, -1, &info);
}

static void _widget_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || g->updating) return;

  if(widget == GTK_WIDGET(g->layer_name))
  {
    _rename_current_layer_from_gui(self, gtk_entry_get_text(g->layer_name));
    return;
  }

  _sync_params_from_gui(self, FALSE);

  if(widget == g->brush_mode || widget == g->brush_shape) _sync_mode_sensitive_widgets(self);

  if(widget == g->size || widget == g->softness || widget == g->brush_shape)
    _sync_temp_buffers(self, TRUE, FALSE);
}

static void _layer_selected(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g || g->updating) return;

  const int active = dt_bauhaus_combobox_get(widget);
  if(active < 0) return;

  const char *text = dt_bauhaus_combobox_get_text(g->layer_select);
  if(!text) return;

  g->updating = TRUE;
  gtk_entry_set_text(g->layer_name, text);
  g->updating = FALSE;

  char previous_name[DRAWLAYER_NAME_SIZE] = { 0 };
  g_strlcpy(previous_name, params->layer_name, sizeof(previous_name));
  const int previous_order = params->layer_order;
  g_strlcpy(params->layer_name, text, sizeof(params->layer_name));
  params->layer_order = active;
  if(!_sync_temp_buffers(self, TRUE, FALSE))
  {
    g_strlcpy(params->layer_name, previous_name, sizeof(params->layer_name));
    params->layer_order = previous_order;
    gui_update(self);
    return;
  }

  g->missing_layer_prompted = FALSE;
  g->missing_layer_prompt_name[0] = '\0';
  _touch_stroke_commit_hash(params, 0);
  if(self->dev)
  {
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    dt_dev_pixelpipe_refresh_all(self->dev, FALSE);
  }
  _refresh_layer_widgets(self);
}

static void _delete_layer_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!self->dev) return;
  if(!_commit_dabs(self, FALSE)) return;
  if(!_confirm_delete_layer(self, FALSE)) return;

  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _invalidate_undo_redo(self);
  _touch_stroke_commit_hash(params, 0);
  self->enabled = FALSE;
  dt_iop_gui_set_enable_button(self);
  if(self->dev) dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  _refresh_layer_widgets(self);
  gui_update(self);
}

static gboolean _fill_current_layer(dt_iop_module_t *self, const float value)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  _cancel_async_resync(self);
  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_sync_temp_buffers(self, FALSE, FALSE)) return FALSE;
  if(!g->base_patch.pixels) return FALSE;

  const size_t count = (size_t)g->base_patch.width * g->base_patch.height;
  _patch_wrlock(&g->base_patch);
  for(size_t k = 0; k < count; k++)
  {
    float *pixel = g->base_patch.pixels + 4 * k;
    pixel[0] = _clamp01(value);
    pixel[1] = _clamp01(value);
    pixel[2] = _clamp01(value);
    pixel[3] = 1.0f;
  }
  _patch_wrunlock(&g->base_patch);

  g->cache_dirty = TRUE;
  _touch_layer_cache_epoch(g);
  _invalidate_process_patch(g);
  _invalidate_undo_redo(self);
  _touch_stroke_commit_hash(params, 0);
  _clear_widget_stroke_locked(g);
  g->live_dirty = FALSE;
  g->preview_drop_pending = FALSE;
  if(g->dabs) g_array_set_size(g->dabs, 0);
  if(g->preview_dabs) g_array_set_size(g->preview_dabs, 0);
  if(g->backend_dabs) g_array_set_size(g->backend_dabs, 0);
  if(g->preview_history) g_array_set_size(g->preview_history, 0);
  if(g->backend_history) g_array_set_size(g->backend_history, 0);
  g->stroke_sample_count = 0;
  _paint_reset_stroke_runtime(g);

  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _clear_current_layer(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  _cancel_async_resync(self);
  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_sync_temp_buffers(self, FALSE, FALSE)) return FALSE;
  if(!g->base_patch.pixels) return FALSE;

  _patch_wrlock(&g->base_patch);
  _clear_transparent_float(g->base_patch.pixels, (size_t)g->base_patch.width * g->base_patch.height);
  _patch_wrunlock(&g->base_patch);

  g->cache_dirty = TRUE;
  _touch_layer_cache_epoch(g);
  _invalidate_process_patch(g);
  _invalidate_undo_redo(self);
  _touch_stroke_commit_hash(params, 0);
  _clear_widget_stroke_locked(g);
  g->live_dirty = FALSE;
  g->preview_drop_pending = FALSE;
  if(g->dabs) g_array_set_size(g->dabs, 0);
  if(g->preview_dabs) g_array_set_size(g->preview_dabs, 0);
  if(g->backend_dabs) g_array_set_size(g->backend_dabs, 0);
  if(g->preview_history) g_array_set_size(g->preview_history, 0);
  if(g->backend_history) g_array_set_size(g->backend_history, 0);
  g->stroke_sample_count = 0;
  _paint_reset_stroke_runtime(g);

  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  dt_control_queue_redraw_center();
  return TRUE;
}

static void _fill_white_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_fill_current_layer(self, 1.0f)) dt_control_log(_("failed to fill drawing layer"));
}

static void _fill_black_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_fill_current_layer(self, 0.0f)) dt_control_log(_("failed to fill drawing layer"));
}

static void _fill_transparent_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_clear_current_layer(self)) dt_control_log(_("failed to clear drawing layer"));
}

static void _save_layer_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !self->dev || !g) return;

  if(!g->cache_valid || !g->base_patch.pixels)
  {
    dt_control_log(_("no drawing layer is loaded in memory"));
    _sync_save_button(self);
    return;
  }
  if(!_layer_name_non_empty(g->cache_layer_name))
  {
    dt_control_log(_("layer name is empty, sidecar save aborted"));
    _refresh_layer_widgets(self);
    return;
  }

  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                             GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_QUESTION,
                                             GTK_BUTTONS_NONE,
                                             "%s",
                                             _("Save the drawing sidecar now?"));
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                           "%s",
                                           _("This writes the current in-memory drawing layer to the sidecar TIFF immediately."));
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Save"), GTK_RESPONSE_ACCEPT);

  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  if(response != GTK_RESPONSE_ACCEPT)
  {
    _sync_save_button(self);
    return;
  }

  /* Saving the sidecar is explicit persistence, not a history edit.
   * We still finalize any pending stroke first so the flush sees the latest
   * authoritative cache state, then write that cache to the TIFF immediately. */
  if(!_commit_dabs(self, FALSE))
  {
    dt_control_log(_("failed to finalize drawing stroke before saving sidecar"));
    _sync_save_button(self);
    return;
  }

  _flush_process_patch_to_base(self, g);
  _rekey_shared_base_patch(&g->base_patch, self->dev->image_storage.id,
                           (const dt_iop_drawlayer_params_t *)self->params);

  if(!_flush_layer_cache(self))
  {
    dt_control_log(_("failed to write drawing layer sidecar"));
    _sync_save_button(self);
    return;
  }

  _release_all_base_patch_extra_refs(g);
  _refresh_layer_widgets(self);

  dt_control_log(_("drawing layer sidecar saved"));

  GtkWidget *done = gtk_message_dialog_new(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                           GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                           GTK_MESSAGE_INFO,
                                           GTK_BUTTONS_OK,
                                           "%s",
                                           _("Drawing sidecar saved."));
  gtk_dialog_run(GTK_DIALOG(done));
  gtk_widget_destroy(done);
}

static void _create_layer_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !g) return;
  if(!_create_new_layer(self, gtk_entry_get_text(g->layer_name)))
    dt_control_log(_("failed to create drawing layer"));
}

static void _create_background_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_create_background_layer_from_input(self))
    dt_control_log(_("failed to create background layer from input"));
}

static void _undo_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_swap_undo_redo(self, TRUE)) dt_control_log(_("failed to undo drawing stroke"));
}

static void _redo_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_swap_undo_redo(self, FALSE)) dt_control_log(_("failed to redo drawing stroke"));
}

static void _preview_bg_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || g->updating || !gtk_toggle_button_get_active(button)) return;

  if(GTK_WIDGET(button) == g->preview_bg_white)
    g->preview_bg_mode = DRAWLAYER_PREVIEW_BG_WHITE;
  else if(GTK_WIDGET(button) == g->preview_bg_grey)
    g->preview_bg_mode = DRAWLAYER_PREVIEW_BG_GREY;
  else if(GTK_WIDGET(button) == g->preview_bg_black)
    g->preview_bg_mode = DRAWLAYER_PREVIEW_BG_BLACK;
  else
    g->preview_bg_mode = DRAWLAYER_PREVIEW_BG_IMAGE;

  _sync_preview_bg_buttons(self);
  if(params) _touch_stroke_commit_hash(params, 0);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  dt_dev_pixelpipe_refresh_all(self->dev, FALSE);
  dt_control_queue_redraw_center();
}

static void _append_dab_sample(dt_iop_module_t *self, const double wx, const double wy, const double pressure)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g) return;
  dt_control_pointer_input_t pointer_input = { 0 };
  dt_control_get_pointer_input(&pointer_input);
  const float pressure_norm = pointer_input.has_pressure ? _clamp01(pointer_input.pressure) : _clamp01(pressure);
  const drawlayer_raw_input_t input = {
    .wx = (float)wx,
    .wy = (float)wy,
    .pressure = pressure_norm,
    .tilt = (float)_clamp01(pointer_input.tilt),
    .acceleration = (float)_clamp01(pointer_input.acceleration),
    .event_ts = g_get_monotonic_time(),
    .stroke_batch = g->current_stroke_batch,
    .stroke_pos = DRAWLAYER_STROKE_MIDDLE,
  };

  if(!_enqueue_input(self, &input)) dt_control_log(_("failed to queue live drawing stroke"));
}

const char *name()
{
  return C_("modulename", "drawing");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("paint premultiplied RGB layers in a TIFF sidecar"),
                                      _("creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric, RGB"),
                                      _("linear, RGB, scene-referred"));
}

#ifdef HAVE_OPENCL
void init_global(dt_iop_module_so_t *module)
{
  const int program = 3; // blendop.cl, from programs.conf
  dt_iop_drawlayer_global_data_t *gd = calloc(1, sizeof(*gd));
  module->data = gd;
  if(!gd) return;
  gd->kernel_premult_over = -1;

  /* Reuse the existing blendop OpenCL program and add one drawlayer-specific
   * kernel to handle premultiplied "over" directly. This avoids the costly
   * de-premultiply + mask split that the stock blend kernels would require. */
  gd->kernel_premult_over = dt_opencl_create_kernel(program, "blendop_premult_over");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_drawlayer_global_data_t *gd = (dt_iop_drawlayer_global_data_t *)module->data;
  if(!gd) return;
  dt_opencl_free_kernel(gd->kernel_premult_over);
  free(gd);
  module->data = NULL;
}
#endif

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_drawlayer_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_drawlayer_params_t));
  module->params_size = sizeof(dt_iop_drawlayer_params_t);
  module->gui_data = NULL;

  if(module->params) ((dt_iop_drawlayer_params_t *)module->params)->layer_order = -1;
  if(module->default_params) ((dt_iop_drawlayer_params_t *)module->default_params)->layer_order = -1;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  (void)self;
  (void)pipe;
  piece->data = calloc(1, sizeof(dt_iop_drawlayer_data_t));
  piece->data_size = sizeof(dt_iop_drawlayer_params_t);
  if(piece->data)
  {
    dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
    data->params.layer_order = -1;
    data->headless_cache_layer_order = -1;
    data->headless_cache_imgid = -1;
  }
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  (void)self;
  (void)pipe;
  if(!piece || !piece->data) return;
  dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
  _clear_headless_cache(data);
  free(piece->data);
  piece->data = NULL;
  piece->data_size = 0;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  (void)pipe;
  if(!piece || !piece->data || !params) return;

  dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
  memcpy(&data->params, params, sizeof(dt_iop_drawlayer_params_t));
  _sanitize_params(self, &data->params);

  /* Every pipe now warms the same authoritative base-patch snapshot through
   * the pixelpipe cache during `commit_params()`. GUI pipes still keep their
   * own transformed ROI cache on top, but they attach to the same shared base
   * line as headless pipes instead of carrying a private sidecar mirror. */
  _refresh_headless_cache(self, data, &data->params, piece);
}

void gui_reset(dt_iop_module_t *self)
{
  if(!self || !self->dev) return;
  if(!_commit_dabs(self, FALSE)) return;
  if(!_confirm_delete_layer(self, FALSE)) return;

  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _default_layer_name(self, params->layer_name, sizeof(params->layer_name));
  params->layer_order = -1;
  _invalidate_undo_redo(self);
  _touch_stroke_commit_hash(params, 0);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  gui_update(self);
  _sync_temp_buffers(self, FALSE, FALSE);
}

gboolean module_will_remove(dt_iop_module_t *self)
{
  if(!self->dev) return TRUE;
  if(!_commit_dabs(self, FALSE)) return FALSE;
  _flush_layer_cache(self);
  return _confirm_delete_layer(self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(drawlayer);
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _ensure_gui_conf_defaults();
  _load_color_history(g);
  _ensure_layer_name(self, params);
  _sanitize_params(self, params);

  g->dabs = g_array_new(FALSE, FALSE, sizeof(drawlayer_dab_t));
  g->preview_dabs = g_array_new(FALSE, FALSE, sizeof(drawlayer_dab_t));
  g->backend_dabs = g_array_new(FALSE, FALSE, sizeof(drawlayer_dab_t));
  g->preview_history = g_array_new(FALSE, FALSE, sizeof(drawlayer_dab_t));
  g->backend_history = g_array_new(FALSE, FALSE, sizeof(drawlayer_dab_t));
  g->preview_path.history = g->preview_history;
  g->backend_path.history = g->backend_history;
  _paint_reset_stroke_runtime(g);
  _rt_init_state(g);
  g->cache_valid = FALSE;
  g->cache_dirty = FALSE;
  g->background_job_running = FALSE;
  g->last_view_x = 0.0f;
  g->last_view_y = 0.0f;
  g->last_view_scale = 1.0f;
  if(self->dev)
  {
    g->last_view_x = self->dev->roi.x;
    g->last_view_y = self->dev->roi.y;
    g->last_view_scale = self->dev->roi.scaling;
  }

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  if(self->reset_button) gtk_widget_hide(self->reset_button);

  GtkWidget *history_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->undo_button = gtk_button_new_with_label(_("undo"));
  g->redo_button = gtk_button_new_with_label(_("redo"));
  g->save_layer = gtk_button_new_with_label(_("save sidecar"));
  gtk_box_pack_start(GTK_BOX(history_box), g->undo_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(history_box), g->redo_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(history_box), g->save_layer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), history_box, FALSE, FALSE, 0);

  GtkWidget *notebook = gtk_notebook_new();
  gtk_widget_set_hexpand(notebook, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), notebook, FALSE, FALSE, 0);

  GtkWidget *brush_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *layer_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *input_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), brush_tab, gtk_label_new(_("Brush")));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), layer_tab, gtk_label_new(_("Layer")));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), input_tab, gtk_label_new(_("Input")));
  gtk_container_child_set(GTK_CONTAINER(notebook), brush_tab, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  gtk_container_child_set(GTK_CONTAINER(notebook), layer_tab, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  gtk_container_child_set(GTK_CONTAINER(notebook), input_tab, "tab-expand", TRUE, "tab-fill", TRUE, NULL);

  GtkWidget *preview_title = gtk_label_new(_("Background"));
  gtk_widget_set_halign(preview_title, GTK_ALIGN_START);
  GtkWidget *preview_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GSList *preview_group = NULL;
  g->preview_bg_image = gtk_radio_button_new_with_label(preview_group, _("image"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->preview_bg_image));
  g->preview_bg_white = gtk_radio_button_new_with_label(preview_group, _("white"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->preview_bg_white));
  g->preview_bg_grey = gtk_radio_button_new_with_label(preview_group, _("grey"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->preview_bg_grey));
  g->preview_bg_black = gtk_radio_button_new_with_label(preview_group, _("black"));
  gtk_box_pack_start(GTK_BOX(preview_box), g->preview_bg_image, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->preview_bg_white, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->preview_bg_grey, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->preview_bg_black, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), preview_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), preview_box, FALSE, FALSE, 0);

  g->brush_mode = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_combobox_add(g->brush_mode, _("paint"));
  dt_bauhaus_combobox_add(g->brush_mode, _("erase"));
  dt_bauhaus_combobox_add(g->brush_mode, _("blur"));
  dt_bauhaus_combobox_add(g->brush_mode, _("smudge"));
  dt_bauhaus_widget_set_label(g->brush_mode, _("Paint mode"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->brush_mode, TRUE, TRUE, 0);

  GtkWidget *color_title = gtk_label_new(_("Color"));
  gtk_label_set_xalign(GTK_LABEL(color_title), 0.0f);
  dt_gui_add_class(color_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), color_title, TRUE, TRUE, 0);
  g->color = gtk_drawing_area_new();
  gtk_widget_set_size_request(g->color, -1, DRAWLAYER_COLOR_PICKER_HEIGHT);
  gtk_widget_add_events(g->color, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
  gtk_box_pack_start(GTK_BOX(brush_tab), g->color, TRUE, TRUE, 0);
  g->color_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(16));
  g->color_swatch = gtk_drawing_area_new();
  gtk_widget_set_size_request(g->color_swatch, -1,
                              DT_PIXEL_APPLY_DPI(DRAWLAYER_COLOR_HISTORY_HEIGHT));
  gtk_widget_add_events(g->color_swatch, GDK_BUTTON_PRESS_MASK);
  gtk_box_pack_start(GTK_BOX(g->color_row), g->color_swatch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(brush_tab), g->color_row, TRUE, TRUE, 0);
  GtkWidget *picker_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(6));
  g->image_colorpicker = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_POINT_AREA, NULL, IOP_CS_NONE);
  g->image_colorpicker_source = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_combobox_add(g->image_colorpicker_source, _("input"));
  dt_bauhaus_combobox_add(g->image_colorpicker_source, _("output"));
  dt_bauhaus_widget_set_label(g->image_colorpicker_source, _("Pick from"));
  gtk_box_pack_start(GTK_BOX(picker_controls), g->image_colorpicker, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(picker_controls), g->image_colorpicker_source, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(brush_tab), picker_controls, TRUE, TRUE, 0);

  g->hdr_exposure = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 4.0f, 0.1f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->hdr_exposure, _("HDR exposure"));
  dt_bauhaus_slider_set_format(g->hdr_exposure, _(" EV"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->hdr_exposure, TRUE, TRUE, 0);

  GtkWidget *geometry_title = gtk_label_new(_("Geometry"));
  gtk_label_set_xalign(GTK_LABEL(geometry_title), 0.0f);
  dt_gui_add_class(geometry_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), geometry_title, TRUE, TRUE, 0);
  g->brush_shape = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_combobox_add(g->brush_shape, _("linear"));
  dt_bauhaus_combobox_add(g->brush_shape, _("gaussian"));
  dt_bauhaus_combobox_add(g->brush_shape, _("quadratic"));
  dt_bauhaus_combobox_add(g->brush_shape, _("sigmoidal"));
  dt_bauhaus_widget_set_label(g->brush_shape, _("Fall-off"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->brush_shape, TRUE, TRUE, 0);

  g->size = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 1.0f, 2048.0f, 1.0f, 64.0f, 0);
  dt_bauhaus_widget_set_label(g->size, _("Size"));
  dt_bauhaus_slider_set_format(g->size, _(" px"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->size, TRUE, TRUE, 0);
  g->distance = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->distance, _("Sampling distance"));
  dt_bauhaus_slider_set_format(g->distance, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->distance, TRUE, TRUE, 0);
  g->smoothing = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->smoothing, _("Smoothing"));
  dt_bauhaus_slider_set_format(g->smoothing, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->smoothing, TRUE, TRUE, 0);
  g->softness = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 1.0f, 0.01f, 0.5f, 2);
  dt_bauhaus_widget_set_label(g->softness, _("Hardness"));
  dt_bauhaus_slider_set_factor(g->softness, 100.0f);
  dt_bauhaus_slider_set_format(g->softness, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->softness, TRUE, TRUE, 0);

  GtkWidget *thickness_title = gtk_label_new(_("Thickness"));
  gtk_label_set_xalign(GTK_LABEL(thickness_title), 0.0f);
  dt_gui_add_class(thickness_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), thickness_title, TRUE, TRUE, 0);
  g->opacity = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 100.0f, 2);
  dt_bauhaus_widget_set_label(g->opacity, _("Opacity"));
  dt_bauhaus_slider_set_format(g->opacity, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->opacity, TRUE, TRUE, 0);
  g->flow = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 100.0f, 2);
  dt_bauhaus_widget_set_label(g->flow, _("Flow"));
  dt_bauhaus_slider_set_format(g->flow, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->flow, TRUE, TRUE, 0);

  GtkWidget *texture_title = gtk_label_new(_("Texture"));
  gtk_label_set_xalign(GTK_LABEL(texture_title), 0.0f);
  dt_gui_add_class(texture_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), texture_title, TRUE, TRUE, 0);
  g->bristles = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->bristles, _("Bristles"));
  dt_bauhaus_slider_set_format(g->bristles, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->bristles, TRUE, TRUE, 0);
  g->bristle_size = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 1.0f, 256.0f, 1.0f, 8.0f, 0);
  dt_bauhaus_widget_set_label(g->bristle_size, _("Bristle size"));
  dt_bauhaus_slider_set_format(g->bristle_size, _(" px"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->bristle_size, TRUE, TRUE, 0);
  g->sprinkles = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->sprinkles, _("Sprinkles"));
  dt_bauhaus_slider_set_format(g->sprinkles, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->sprinkles, TRUE, TRUE, 0);
  g->sprinkle_size = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 1.0f, 256.0f, 1.0f, 3.0f, 0);
  dt_bauhaus_widget_set_label(g->sprinkle_size, _("Sprinkle size"));
  dt_bauhaus_slider_set_format(g->sprinkle_size, _(" px"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->sprinkle_size, TRUE, TRUE, 0);

  GtkWidget *layer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *layer_name_title = gtk_label_new(_("Layer name"));
  gtk_widget_set_halign(layer_name_title, GTK_ALIGN_START);
  GtkWidget *layer_fill_title = gtk_label_new(_("Fill"));
  gtk_widget_set_halign(layer_fill_title, GTK_ALIGN_START);
  GtkWidget *layer_action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *layer_fill_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->layer_name = GTK_ENTRY(gtk_entry_new());
  dt_accels_disconnect_on_text_input(GTK_WIDGET(g->layer_name));
  g->layer_select = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->layer_select, _("Source layer"));
  g->delete_layer = gtk_button_new_with_label(_("delete layer"));
  g->create_layer = gtk_button_new_with_label(_("create new layer"));
  g->create_background = gtk_button_new_with_label(_("create background from input"));
  g->fill_white = gtk_button_new_with_label(_("white"));
  g->fill_black = gtk_button_new_with_label(_("black"));
  g->fill_transparent = gtk_button_new_with_label(_("transparency"));
  gtk_box_pack_start(GTK_BOX(layer_box), layer_name_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), GTK_WIDGET(g->layer_name), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), g->layer_select, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_action_row), g->create_layer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_action_row), g->delete_layer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_action_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_fill_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->fill_white, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->fill_black, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->fill_transparent, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_fill_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), g->create_background, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), layer_box, FALSE, FALSE, 0);

  GtkWidget *mapping_title = gtk_label_new(_("tablet mapping"));
  gtk_widget_set_halign(mapping_title, GTK_ALIGN_START);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 6);

  const char *labels[4] = { _("size"), _("opacity"), _("flow"), _("hardness") };
  const char *rows[3] = { _("pressure"), _("tilt"), _("acceleration") };
  GtkWidget **targets[3][4] = {
    { &g->map_pressure_size, &g->map_pressure_opacity, &g->map_pressure_flow, &g->map_pressure_softness },
    { &g->map_tilt_size, &g->map_tilt_opacity, &g->map_tilt_flow, &g->map_tilt_softness },
    { &g->map_accel_size, &g->map_accel_opacity, &g->map_accel_flow, &g->map_accel_softness },
  };
  GtkWidget **profiles[3] = { &g->pressure_profile, &g->tilt_profile, &g->accel_profile };

  for(int c = 0; c < 4; c++)
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(labels[c]), c + 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("profile")), 5, 0, 1, 1);

  for(int r = 0; r < 3; r++)
  {
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(rows[r]), 0, r + 1, 1, 1);
    for(int c = 0; c < 4; c++)
    {
      *targets[r][c] = gtk_check_button_new();
      gtk_grid_attach(GTK_GRID(grid), *targets[r][c], c + 1, r + 1, 1, 1);
    }
    *profiles[r] = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
    dt_bauhaus_combobox_add(*profiles[r], _("linear"));
    dt_bauhaus_combobox_add(*profiles[r], _("quadratic"));
    dt_bauhaus_combobox_add(*profiles[r], _("square root"));
    gtk_grid_attach(GTK_GRID(grid), *profiles[r], 5, r + 1, 1, 1);
  }

  gtk_box_pack_start(GTK_BOX(input_tab), mapping_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(input_tab), grid, FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(g->brush_shape), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->brush_mode), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(g->color, "draw", G_CALLBACK(_color_picker_draw), self);
  g_signal_connect(g->color_swatch, "draw", G_CALLBACK(_color_swatch_draw), self);
  g_signal_connect(g->color_swatch, "button-press-event", G_CALLBACK(_color_swatch_button_press), self);
  g_signal_connect(g->color, "button-press-event", G_CALLBACK(_color_picker_button_press), self);
  g_signal_connect(g->color, "button-release-event", G_CALLBACK(_color_picker_button_release), self);
  g_signal_connect(g->color, "motion-notify-event", G_CALLBACK(_color_picker_motion), self);
  g_signal_connect(G_OBJECT(g->image_colorpicker_source), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->size), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->distance), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->smoothing), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->opacity), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->flow), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->bristles), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->bristle_size), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->sprinkles), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->sprinkle_size), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->softness), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->hdr_exposure), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(g->layer_name, "changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->layer_select), "value-changed", G_CALLBACK(_layer_selected), self);
  g_signal_connect(g->undo_button, "clicked", G_CALLBACK(_undo_clicked), self);
  g_signal_connect(g->redo_button, "clicked", G_CALLBACK(_redo_clicked), self);
  g_signal_connect(g->preview_bg_image, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->preview_bg_white, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->preview_bg_grey, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->preview_bg_black, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->create_layer, "clicked", G_CALLBACK(_create_layer_clicked), self);
  g_signal_connect(g->create_background, "clicked", G_CALLBACK(_create_background_clicked), self);
  g_signal_connect(g->save_layer, "clicked", G_CALLBACK(_save_layer_clicked), self);
  g_signal_connect(g->delete_layer, "clicked", G_CALLBACK(_delete_layer_clicked), self);
  g_signal_connect(g->fill_white, "clicked", G_CALLBACK(_fill_white_clicked), self);
  g_signal_connect(g->fill_black, "clicked", G_CALLBACK(_fill_black_clicked), self);
  g_signal_connect(g->fill_transparent, "clicked", G_CALLBACK(_fill_transparent_clicked), self);

  for(int r = 0; r < 3; r++)
  {
    for(int c = 0; c < 4; c++)
      g_signal_connect(*targets[r][c], "toggled", G_CALLBACK(_widget_changed), self);
    g_signal_connect(G_OBJECT(*profiles[r]), "value-changed", G_CALLBACK(_widget_changed), self);
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                                  G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  gui_update(self);
  if(self->dev) _sync_temp_buffers(self, FALSE, FALSE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g) return;

  _ensure_layer_name(self, params);
  _sanitize_params(self, params);
  g->updating = TRUE;

  dt_bauhaus_combobox_set(g->brush_shape, _conf_brush_shape());
  dt_bauhaus_combobox_set(g->brush_mode, _conf_brush_mode());
  dt_bauhaus_slider_set(g->size, _conf_size());
  dt_bauhaus_slider_set(g->distance, _conf_distance());
  dt_bauhaus_slider_set(g->smoothing, _conf_smoothing());
  dt_bauhaus_slider_set(g->opacity, _conf_opacity());
  dt_bauhaus_slider_set(g->flow, _conf_flow());
  dt_bauhaus_slider_set(g->bristles, _conf_bristles());
  dt_bauhaus_slider_set(g->bristle_size, _conf_bristle_size());
  dt_bauhaus_slider_set(g->sprinkles, _conf_sprinkles());
  dt_bauhaus_slider_set(g->sprinkle_size, _conf_sprinkle_size());
  dt_bauhaus_slider_set(g->softness, _conf_hardness());
  if(g->image_colorpicker_source) dt_bauhaus_combobox_set(g->image_colorpicker_source, _conf_pick_source());
  dt_bauhaus_slider_set(g->hdr_exposure, _conf_hdr_exposure());

  _sync_color_picker_from_conf(self);
  if(g->color) gtk_widget_queue_draw(g->color);
  gtk_entry_set_text(g->layer_name, params->layer_name);

  _set_toggle_if_valid(g->map_pressure_size, _conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE));
  _set_toggle_if_valid(g->map_pressure_opacity, _conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY));
  _set_toggle_if_valid(g->map_pressure_flow, _conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW));
  _set_toggle_if_valid(g->map_pressure_softness, _conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS));

  _set_toggle_if_valid(g->map_tilt_size, _conf_bool(DRAWLAYER_CONF_MAP_TILT_SIZE));
  _set_toggle_if_valid(g->map_tilt_opacity, _conf_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY));
  _set_toggle_if_valid(g->map_tilt_flow, _conf_bool(DRAWLAYER_CONF_MAP_TILT_FLOW));
  _set_toggle_if_valid(g->map_tilt_softness, _conf_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS));

  _set_toggle_if_valid(g->map_accel_size, _conf_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE));
  _set_toggle_if_valid(g->map_accel_opacity, _conf_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY));
  _set_toggle_if_valid(g->map_accel_flow, _conf_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW));
  _set_toggle_if_valid(g->map_accel_softness, _conf_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS));

  if(g->pressure_profile) dt_bauhaus_combobox_set(g->pressure_profile, _conf_mapping_profile(DRAWLAYER_CONF_PRESSURE_PROFILE));
  if(g->tilt_profile) dt_bauhaus_combobox_set(g->tilt_profile, _conf_mapping_profile(DRAWLAYER_CONF_TILT_PROFILE));
  if(g->accel_profile) dt_bauhaus_combobox_set(g->accel_profile, _conf_mapping_profile(DRAWLAYER_CONF_ACCEL_PROFILE));

  _sync_mode_sensitive_widgets(self);
  _sync_undo_redo_buttons(self);
  _sync_preview_bg_buttons(self);
  _sync_save_button(self);
  _populate_layer_list(self);
  g->updating = FALSE;
}

/*
 * Drawlayer keeps the visible darkroom pipes in best-effort mode while the
 * module owns the focus so long-running interactive updates are not cancelled
 * mid-flight by the normal shutdown kill-switches. We also bypass the module
 * cache in that same window so every in-stroke recompute evaluates the current
 * cache buffer instead of short-circuiting through stale cache lines.
 */
static void _set_drawlayer_interactive_pipeline_mode(dt_iop_module_t *self, gboolean state)
{
  if(!self) return;

  dt_iop_set_cache_bypass(self, state);

  if(!self->dev) return;

  dt_dev_pixelpipe_set_realtime(self->dev->pipe, state);
  dt_dev_pixelpipe_set_realtime(self->dev->preview_pipe, state);
  dt_dev_pixelpipe_set_realtime(self->dev->virtual_pipe, state);
}

void change_image(dt_iop_module_t *self)
{
  if(self->gui_data)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    _cancel_async_resync(self);
    _commit_dabs(self, FALSE);
    if(g) _flush_process_patch_to_base(self, g);
    _flush_layer_cache(self);
    _stop_worker(self);
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    g->cache_valid = FALSE;
    g->cache_dirty = FALSE;
    _sync_save_button(self);
    gui_update(self);
    _sync_temp_buffers(self, TRUE, FALSE);
    if(self->dev && self->dev->gui_module == self && !_start_worker(self))
      dt_control_log(_("failed to restart drawing worker"));
  }
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  if(!in)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
    const int pending_samples = g ? (int)g->stroke_sample_count : 0;
    const gboolean had_pending_edits
        = (g && (g->cache_dirty || g->process_patch_dirty || g->stroke.active
                 || g->pending_stroke_applied || g->stroke_sample_count > 0));
    _set_drawlayer_os_cursor_hidden(FALSE);
    _cancel_async_resync(self);
    /* On focus loss we want exactly one final coherent state pushed to history:
     * 1) drain workers/queues without creating intermediate history entries,
     * 2) fold process tile into authoritative base,
     * 3) flush sidecar,
     * 4) bump hash + history once if anything changed. */
    _commit_dabs(self, FALSE);
    if(had_pending_edits && params)
      _touch_stroke_commit_hash(params, pending_samples);
    if(g) _flush_process_patch_to_base(self, g);
    if(!_flush_layer_cache(self)) dt_control_log(_("failed to write drawing layer sidecar"));
    if(had_pending_edits && self->dev && params)
      dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    _stop_worker(self);
    _set_drawlayer_interactive_pipeline_mode(self, FALSE);
  }
  else if(self->gui_data)
  {
    _set_drawlayer_interactive_pipeline_mode(self, TRUE);
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
    if(g)
    {
      g->missing_layer_prompted = FALSE;
      g->missing_layer_prompt_name[0] = '\0';
    }

    if(!_start_worker(self)) dt_control_log(_("failed to start drawing worker"));
    _sync_temp_buffers(self, FALSE, FALSE);
    _sync_save_button(self);

    if(g && params && !g->cache_valid && _current_layer_missing_in_sidecar(self))
    {
      const int action = _offer_missing_layer_recreation(self, params->layer_name);
      if(action == 2 && !_create_new_layer(self, params->layer_name))
        dt_control_log(_("failed to create drawing layer"));
    }
  }
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  _set_drawlayer_os_cursor_hidden(FALSE);
  _cancel_async_resync(self);
  _rt_cancel_async_undo_ui(self);
  _commit_dabs(self, FALSE);
  if(g) _flush_process_patch_to_base(self, g);
  _flush_layer_cache(self);
  _stop_worker(self);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  if(g && g->dabs)
  {
    g_array_free(g->dabs, TRUE);
    g->dabs = NULL;
  }
  if(g && g->preview_dabs)
  {
    g_array_free(g->preview_dabs, TRUE);
    g->preview_dabs = NULL;
  }
  if(g && g->backend_dabs)
  {
    g_array_free(g->backend_dabs, TRUE);
    g->backend_dabs = NULL;
  }
  if(g && g->preview_history)
  {
    g_array_free(g->preview_history, TRUE);
    g->preview_history = NULL;
  }
  if(g && g->backend_history)
  {
    g_array_free(g->backend_history, TRUE);
    g->backend_history = NULL;
  }
  if(g)
  {
    g_free(g->preview_path.smudge_pixels);
    g->preview_path.smudge_pixels = NULL;
    g->preview_path.smudge_width = 0;
    g->preview_path.smudge_height = 0;
    g_free(g->preview_path.bristle_map);
    g->preview_path.bristle_map = NULL;
    g->preview_path.bristle_map_size = 0;
    g->preview_path.bristle_map_seed = 0u;
    g->preview_path.bristle_map_cached_seed = 0u;
    g_free(g->backend_path.smudge_pixels);
    g->backend_path.smudge_pixels = NULL;
    g->backend_path.smudge_width = 0;
    g->backend_path.smudge_height = 0;
    g_free(g->backend_path.bristle_map);
    g->backend_path.bristle_map = NULL;
    g->backend_path.bristle_map_size = 0;
    g->backend_path.bristle_map_seed = 0u;
    g->backend_path.bristle_map_cached_seed = 0u;
    g_free(g->process_path.smudge_pixels);
    g->process_path.smudge_pixels = NULL;
    g->process_path.smudge_width = 0;
    g->process_path.smudge_height = 0;
    g_free(g->process_path.bristle_map);
    g->process_path.bristle_map = NULL;
    g->process_path.bristle_map_size = 0;
    g->process_path.bristle_map_seed = 0u;
    g->process_path.bristle_map_cached_seed = 0u;
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    _clear_patch(&g->process_patch);
    _clear_patch(&g->process_update_patch);
    _clear_patch(&g->undo_patch);
    _free_tracked_temp_buffer((void **)&g->stroke_mask, "drawlayer stroke mask");
    g->stroke_mask_width = 0;
    g->stroke_mask_height = 0;
    _free_tracked_temp_buffer((void **)&g->process_stroke_mask, "drawlayer process stroke mask");
    g->process_stroke_mask_width = 0;
    g->process_stroke_mask_height = 0;
    _clear_patch(&g->live_patch);
    _clear_live_surface(g);
    _clear_color_picker_surface(g);
    _clear_cursor_stamp_surface(g);
    g->cache_valid = FALSE;
    g->cache_dirty = FALSE;
    g->process_patch_valid = FALSE;
    g->process_geom_hash = 0;
    _rt_cleanup_state(g);
  }

  IOP_GUI_FREE;
}

typedef struct drawlayer_hud_brush_state_t
{
  float pressure;
  float tilt;
  float acceleration;
  float radius;
  float opacity;
  float flow;
  float hardness;
} drawlayer_hud_brush_state_t;

static void _compute_hud_brush_state(const dt_control_pointer_input_t *pointer_input,
                                     drawlayer_hud_brush_state_t *state)
{
  if(!state) return;

  const float pressure_norm = _clamp01((pointer_input && pointer_input->has_pressure) ? pointer_input->pressure : 1.0f);
  const float tilt_norm = _clamp01((pointer_input && pointer_input->has_tilt) ? pointer_input->tilt : 0.0f);
  const float accel_norm = _clamp01(pointer_input ? pointer_input->acceleration : 0.0f);
  const drawlayer_mapping_profile_t pressure_profile = _conf_mapping_profile(DRAWLAYER_CONF_PRESSURE_PROFILE);
  const drawlayer_mapping_profile_t tilt_profile = _conf_mapping_profile(DRAWLAYER_CONF_TILT_PROFILE);
  const drawlayer_mapping_profile_t accel_profile = _conf_mapping_profile(DRAWLAYER_CONF_ACCEL_PROFILE);
  const float pressure_coeff = _mapping_multiplier(pressure_profile, pressure_norm);
  const float tilt_coeff = _mapping_multiplier(tilt_profile, tilt_norm);
  const float accel_coeff = _mapping_multiplier(accel_profile, accel_norm);

  float radius = _conf_size();
  float opacity = _conf_opacity() / 100.0f;
  float flow = _conf_flow() / 100.0f;
  float hardness = _conf_hardness();

  if(_conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE)) radius *= pressure_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY)) opacity *= pressure_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW)) flow *= pressure_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS)) hardness *= pressure_coeff;

  if(_conf_bool(DRAWLAYER_CONF_MAP_TILT_SIZE)) radius *= tilt_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY)) opacity *= tilt_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_TILT_FLOW)) flow *= tilt_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS)) hardness *= tilt_coeff;

  if(_conf_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE)) radius *= accel_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY)) opacity *= accel_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW)) flow *= accel_coeff;
  if(_conf_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS)) hardness *= accel_coeff;

  state->pressure = pressure_norm;
  state->tilt = tilt_norm;
  state->acceleration = accel_norm;
  state->radius = fmaxf(0.5f, radius);
  state->opacity = _clamp01(opacity);
  state->flow = _clamp01(flow);
  state->hardness = _clamp01(hardness);
}

static void _draw_brush_hud(cairo_t *cr, const drawlayer_hud_brush_state_t *state)
{
  if(!cr || !state) return;

  char lines[3][128] = { { 0 } };
  g_snprintf(lines[0], sizeof(lines[0]), _("size %.1f px  hardness %.2f%%"),
             state->radius * 2.0f, state->hardness * 100.0f);
  g_snprintf(lines[1], sizeof(lines[1]), _("opacity %.2f%%  flow %.2f%%"),
             state->opacity * 100.0f, state->flow * 100.0f);
  g_snprintf(lines[2], sizeof(lines[2]), _("pressure %.2f%%  tilt %.2f%%  acceleration %.2f%%"),
             state->pressure * 100.0f, state->tilt * 100.0f, state->acceleration * 100.0f);

  const double pad = DT_PIXEL_APPLY_DPI(6.0);
  const double line_h = DT_PIXEL_APPLY_DPI(13.0);
  const double fs = DT_PIXEL_APPLY_DPI(12.0);
  double max_w = 0.0;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, fs);
  for(int i = 0; i < 3; i++)
  {
    cairo_text_extents_t ext = { 0 };
    cairo_text_extents(cr, lines[i], &ext);
    max_w = fmax(max_w, ext.x_advance);
  }

  const double box_w = max_w + 2.0 * pad;
  const double box_h = 3.0 * line_h + 2.0 * pad;
  const double x = DT_PIXEL_APPLY_DPI(10.0);
  const double y = DT_PIXEL_APPLY_DPI(10.0);

  cairo_save(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
  cairo_rectangle(cr, x, y, box_w, box_h);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
  for(int i = 0; i < 3; i++)
  {
    cairo_move_to(cr, x + pad, y + pad + (i + 1) * line_h - DT_PIXEL_APPLY_DPI(2.0));
    cairo_show_text(cr, lines[i]);
  }
  cairo_restore(cr);
}

void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  (void)pointerx;
  (void)pointery;

  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return;

  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);

  _paint_temp_buffer(self, cr, width, height);

  if(g->pointer_valid)
  {
    float radius = _conf_size();
    const int brush_mode = _conf_brush_mode();
    const gboolean show_paint_fill = (brush_mode == DT_IOP_DRAWLAYER_MODE_PAINT);

    float draw_x = g->pointer_x;
    float draw_y = g->pointer_y;
    float lx = 0.0f;
    float ly = 0.0f;
    float widget_radius = radius * dt_dev_get_overlay_scale(self->dev);
    if(_widget_to_layer_coords(self, g->pointer_x, g->pointer_y, &lx, &ly))
    {
      drawlayer_dab_t dab = {
        .x = lx,
        .y = ly,
        .radius = radius,
      };
      if(!_layer_to_widget_coords(self, lx, ly, &draw_x, &draw_y))
      {
        draw_x = g->pointer_x;
        draw_y = g->pointer_y;
      }
      widget_radius = _widget_brush_radius(self, &dab, widget_radius);
    }

    // Draw the brush mipmap
    const float draw_radius = fmaxf(0.5f, widget_radius);
    if(show_paint_fill)
    {
      _ensure_cursor_stamp_surface(self, draw_radius);
      if(g->cursor_surface)
      {
        cairo_set_source_surface(cr, g->cursor_surface, draw_x - draw_radius - 1.0f, draw_y - draw_radius - 1.0f);
        cairo_paint(cr);
      }
    }

    // Draw the outer circle in case we are lost
    cairo_set_source_rgba(cr, 0., 0., 0., 0.5);
    cairo_set_line_width(cr, 2.5);
    cairo_arc(cr, draw_x, draw_y, draw_radius + 1.0f, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 1., 1., 1., 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, draw_x, draw_y, draw_radius, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    if(self->dev->gui_module == self)
    {
      dt_control_pointer_input_t pointer_input = { 0 };
      drawlayer_hud_brush_state_t hud = { 0 };
      dt_control_get_pointer_input(&pointer_input);
      _compute_hud_brush_state(&pointer_input, &hud);
      _draw_brush_hud(cr, &hud);
    }
  }

  cairo_restore(cr);
}

int mouse_leave(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g) return 0;
  if(!g->painting && g->cache_valid && g->process_patch_valid && g->process_patch.pixels)
  {
    /* Persist the display-sized process tile back into authoritative cache when
     * leaving the drawing area, keeping heavy commits off the active draw loop. */
    _flush_process_patch_to_base(self, g);
  }
  g->pointer_valid = FALSE;
  _set_drawlayer_os_cursor_hidden(FALSE);
  dt_control_queue_redraw_center();
  return 0;
}

int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return 0;

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    /* When the standard image color picker is active, drawlayer must stop
     * capturing the pointer entirely so darkroom can drive the picker overlay
     * and sampling path without competing cursor state from the brush tool. */
    const gboolean had_pointer = g->pointer_valid;
    g->pointer_valid = FALSE;
    if(g->painting)
    {
      g->painting = FALSE;
      _sync_undo_redo_buttons(self);
    }
    _set_drawlayer_os_cursor_hidden(FALSE);
    if(had_pointer) dt_control_queue_redraw_center();
    return 0;
  }

  if(!g->pointer_valid) _set_drawlayer_os_cursor_hidden(TRUE);
  g->pointer_valid = TRUE;
  g->pointer_x = (float)x;
  g->pointer_y = (float)y;

  if(g->painting) _append_dab_sample(self, x, y, pressure);
  else
    dt_control_queue_redraw_center();
  return g->painting ? 1 : 0;
}

int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || which != 1) return 0;

  if(self->dev->gui_module != self)
  {
    dt_iop_request_focus(self);
    return 1;
  }

  _cancel_async_resync(self);
  if(!_sync_temp_buffers(self, TRUE, FALSE)) return 0;
  /* Prepare undo snapshot on stroke start in the UI thread (once per stroke),
   * not in the backend worker per first-sample callback. This keeps backend
   * sample processing focused on dab rasterization only.
   */
  _prepare_undo_snapshot(self);
  if(!_start_worker(self)) return 0;

  dt_iop_gui_enter_critical_section(self);
  g->painting = TRUE;
  g->pointer_valid = TRUE;
  g->pointer_x = (float)x;
  g->pointer_y = (float)y;
  g->current_stroke_batch = ++g->next_stroke_batch;
  if(g->current_stroke_batch == 0) g->current_stroke_batch = ++g->next_stroke_batch;
  g_array_set_size(g->dabs, 0);
  if(!_rt_workers_active(g))
  {
    _clear_widget_stroke_locked(g);
    _paint_clear_stroke_state(&g->stroke);
  }
  g->live_mode = _conf_brush_mode();
  g->live_dirty = FALSE;
  if(!g->preview_drop_pending) g->finish_commit_pending = FALSE;
  g->live_surface_dirty = FALSE;
  g->live_surface_damage_valid = FALSE;
  g->stroke_damage_valid = FALSE;
  g->stroke_sample_count = 0;
  _sync_undo_redo_buttons(self);
  dt_iop_gui_leave_critical_section(self);
  dt_control_pointer_input_t pointer_input = { 0 };
  dt_control_get_pointer_input(&pointer_input);
  const float pressure_norm = pointer_input.has_pressure ? _clamp01(pointer_input.pressure) : _clamp01(pressure);
  const drawlayer_raw_input_t first = {
    .wx = (float)x,
    .wy = (float)y,
    .pressure = pressure_norm,
    .tilt = (float)_clamp01(pointer_input.tilt),
    .acceleration = (float)_clamp01(pointer_input.acceleration),
    .event_ts = g_get_monotonic_time(),
    .stroke_batch = g->current_stroke_batch,
    .stroke_pos = DRAWLAYER_STROKE_FIRST,
  };
  if(!_enqueue_input(self, &first)) dt_control_log(_("failed to queue live drawing stroke"));
  dt_control_queue_redraw_center();
  return 1;
}

int button_released(dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || which != 1) return 0;

  if(g->painting)
  {
    g->painting = FALSE;
    _sync_undo_redo_buttons(self);
    if(!_enqueue_stroke_end(self)) dt_control_log(_("failed to queue drawing stroke end"));
    return 1;
  }

  return 0;
}

int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  if(!self->dev || self->dev->gui_module != self) return 0;

  const gboolean increase = dt_mask_scroll_increases(up);
  const float factor = increase ? 1.1f : 0.9f;
  const float new_size = CLAMP(_conf_size() * factor, 1.0f, 2048.0f);
  dt_conf_set_float(DRAWLAYER_CONF_SIZE, new_size);

  if(self->gui_data)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    g->updating = TRUE;
    dt_bauhaus_slider_set(g->size, new_size);
    g->updating = FALSE;
  }

  _sync_temp_buffers(self, TRUE, FALSE);
  dt_control_queue_redraw_center();
  return 1;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const gint64 process_t0 = g_get_monotonic_time();
  gint64 process_t = process_t0;
  const dt_iop_drawlayer_global_data_t *gd = (const dt_iop_drawlayer_global_data_t *)self->global_data;
  const dt_iop_drawlayer_data_t *data = (const dt_iop_drawlayer_data_t *)piece->data;
  const dt_iop_drawlayer_params_t *runtime_params = data ? &data->params : (const dt_iop_drawlayer_params_t *)self->params;
  if(!gd || gd->kernel_premult_over < 0) return FALSE;
  if(!runtime_params || runtime_params->layer_name[0] == '\0')
    return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;

  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  int preview_bg_mode = DRAWLAYER_PREVIEW_BG_IMAGE;
  if(g && self->dev && self->dev->gui_module == self) preview_bg_mode = g->preview_bg_mode;
  const gboolean use_preview_bg = (preview_bg_mode != DRAWLAYER_PREVIEW_BG_IMAGE);
  const float preview_bg = (preview_bg_mode == DRAWLAYER_PREVIEW_BG_WHITE)
                               ? 1.0f
                               : (preview_bg_mode == DRAWLAYER_PREVIEW_BG_GREY) ? 0.5f : 0.0f;
  const int current_full_w = piece->pipe->iwidth;
  const int current_full_h = piece->pipe->iheight;
  if(current_full_w <= 0 || current_full_h <= 0)
    return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;

  drawlayer_process_scratch_t *scratch = _get_process_scratch();
  if(!scratch) return FALSE;

  if(g && _is_drawlayer_display_pipe(self, piece))
  {
    const int cache_ref_w = g->cache_raw_width > 0 ? g->cache_raw_width : piece->pipe->iwidth;
    const int cache_ref_h = g->cache_raw_height > 0 ? g->cache_raw_height : piece->pipe->iheight;
    const gboolean have_cache = _layer_cache_matches(g, piece->pipe->image.id, cache_ref_w, cache_ref_h,
                                                     runtime_params->layer_name, runtime_params->layer_order);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=raw-cache-check ms=%.3f hit=%d",
               (now - process_t) / 1000.0, have_cache ? 1 : 0);
      process_t = now;
    }
    if(have_cache && _build_process_patch_from_base(self, g, piece, roi_in, roi_out))
    {
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=prepare-process-patch ms=%.3f",
                 (now - process_t) / 1000.0);
        process_t = now;
      }
      if(!g->process_patch.pixels || g->process_patch.width <= 0 || g->process_patch.height <= 0)
        return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;

      const gboolean direct_copy = (g->process_patch.width == roi_out->width
                                    && g->process_patch.height == roi_out->height);
      const dt_iop_roi_t source_process_roi = {
        .x = 0,
        .y = 0,
        .width = g->process_patch.width,
        .height = g->process_patch.height,
        .scale = 1.0f,
      };
      const dt_iop_roi_t blend_target_roi = *roi_out;
      const gboolean ok = _blend_layer_over_input_cl(piece->pipe->devid, gd->kernel_premult_over, dev_out, dev_in,
                                                     scratch, g->process_patch.pixels, g->process_patch.cache_entry,
                                                     g->process_patch.width, g->process_patch.height,
                                                     &blend_target_roi, &source_process_roi, direct_copy,
                                                     use_preview_bg, preview_bg);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=blend-process-patch ms=%.3f total=%.3f ok=%d",
                 (now - process_t) / 1000.0, (now - process_t0) / 1000.0, ok ? 1 : 0);
      }
      return ok;
    }
  }

  if(data && data->headless_cache_valid && data->headless_base_patch.pixels
     && data->headless_cache_imgid == piece->pipe->image.id)
  {
    const dt_iop_roi_t process_roi = roi_in ? *roi_in : *roi_out;
    const dt_iop_roi_t source_full_roi = {
      .x = 0,
      .y = 0,
      .width = data->headless_base_patch.width,
      .height = data->headless_base_patch.height,
      .scale = 1.0f,
    };
    dt_iop_roi_t combined_roi = { 0 };
    _build_combined_process_roi(piece, &process_roi, current_full_w, current_full_h,
                                data->headless_base_patch.width, data->headless_base_patch.height, &combined_roi);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=build-combined-roi-headless ms=%.3f",
               (now - process_t) / 1000.0);
      process_t = now;
    }
    _patch_rdlock(&data->headless_base_patch);
    const gboolean ok = _blend_layer_over_input_cl(piece->pipe->devid, gd->kernel_premult_over, dev_out, dev_in,
                                                    scratch, data->headless_base_patch.pixels,
                                                    data->headless_base_patch.cache_entry,
                                                    data->headless_base_patch.width,
                                                    data->headless_base_patch.height, &combined_roi,
                                                    &source_full_roi, FALSE, use_preview_bg, preview_bg);
    _patch_rdunlock(&data->headless_base_patch);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=blend-headless ms=%.3f total=%.3f ok=%d",
               (now - process_t) / 1000.0, (now - process_t0) / 1000.0, ok ? 1 : 0);
    }
    return ok;
  }

  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=no-cache-pass-through ms=%.3f total=%.3f",
             (now - process_t) / 1000.0, (now - process_t0) / 1000.0);
  }
  return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;
}
#endif

int process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
            const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_drawlayer_data_t *data = (const dt_iop_drawlayer_data_t *)piece->data;
  const dt_iop_drawlayer_params_t *runtime_params = data ? &data->params : (const dt_iop_drawlayer_params_t *)self->params;
  const float *input = (const float *)ivoid;
  float *output = (float *)ovoid;
  const size_t pixels = (size_t)roi_out->width * roi_out->height;
  const gint64 process_t0 = g_get_monotonic_time();
  gint64 process_t = process_t0;

#ifndef DRAWLAYER_PROCESS_TRACE
#define DRAWLAYER_PROCESS_TRACE 0
#endif
#if DRAWLAYER_PROCESS_TRACE
  dt_print(DT_DEBUG_DEV,
           "[drawlayer] process pipe_in=%dx%d piece_buf_in=(x=%d y=%d w=%d h=%d scale=%.6f) "
           "piece_buf_out=(x=%d y=%d w=%d h=%d scale=%.6f) "
           "roi_in=(x=%d y=%d w=%d h=%d scale=%.6f) "
           "roi_out=(x=%d y=%d w=%d h=%d scale=%.6f)\n",
           piece->pipe->iwidth, piece->pipe->iheight, piece->buf_in.x, piece->buf_in.y, piece->buf_in.width,
           piece->buf_in.height, piece->buf_in.scale, piece->buf_out.x, piece->buf_out.y, piece->buf_out.width,
           piece->buf_out.height, piece->buf_out.scale, roi_in ? roi_in->x : 0, roi_in ? roi_in->y : 0,
           roi_in ? roi_in->width : 0, roi_in ? roi_in->height : 0, roi_in ? roi_in->scale : 0.0f,
           roi_out ? roi_out->x : 0, roi_out ? roi_out->y : 0, roi_out ? roi_out->width : 0,
           roi_out ? roi_out->height : 0, roi_out ? roi_out->scale : 0.0f);
#endif

  /* `process()` keeps the pipeline contract simple:
   * - the in-memory cache and stroke math stay in float32,
   * - TIFF I/O converts to/from half-float only at the file boundary. */
  if(!runtime_params || runtime_params->layer_name[0] == '\0')
  {
    _copy_input_to_output(input, output, roi_out->width, roi_out->height);
    return 0;
  }

  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  int preview_bg_mode = DRAWLAYER_PREVIEW_BG_IMAGE;
  if(g && self->dev && self->dev->gui_module == self) preview_bg_mode = g->preview_bg_mode;
  const gboolean use_preview_bg = (preview_bg_mode != DRAWLAYER_PREVIEW_BG_IMAGE);
  const float preview_bg = (preview_bg_mode == DRAWLAYER_PREVIEW_BG_WHITE)
                               ? 1.0f
                               : (preview_bg_mode == DRAWLAYER_PREVIEW_BG_GREY) ? 0.5f : 0.0f;
  const int current_full_w = piece->pipe->iwidth;
  const int current_full_h = piece->pipe->iheight;
  if(current_full_w <= 0 || current_full_h <= 0)
  {
    _copy_input_to_output(input, output, roi_out->width, roi_out->height);
    return 0;
  }
  if(g && _is_drawlayer_display_pipe(self, piece))
  {
    /* Fastest steady-state path:
     * - raw/full-resolution layer cache is already resident,
     * - transformed process tile for the current geometry is either already
     *   valid or can be rebuilt once from that raw cache,
     * - then `process()` only blends the prepared tile.
     *
     * This intentionally bypasses the older one-shot post-zoom pixelpipe cache.
     * That cache still helps as a fallback, but it cannot remove the dominant
     * cost because it is repopulated once per snapshot. `process_patch` is the
     * persistent transformed tile that the backend worker can update in place. */
    const int cache_ref_w = g->cache_raw_width > 0 ? g->cache_raw_width : piece->pipe->iwidth;
    const int cache_ref_h = g->cache_raw_height > 0 ? g->cache_raw_height : piece->pipe->iheight;
    const gboolean have_cache = _layer_cache_matches(g, piece->pipe->image.id, cache_ref_w, cache_ref_h,
                                                     runtime_params->layer_name, runtime_params->layer_order);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process step=raw-cache-check ms=%.3f hit=%d",
               (now - process_t) / 1000.0, have_cache ? 1 : 0);
      process_t = now;
    }
    if(have_cache && _build_process_patch_from_base(self, g, piece, roi_in, roi_out))
    {
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-patch ms=%.3f",
                 (now - process_t) / 1000.0);
        process_t = now;
      }
      const gboolean direct_copy = (g->process_patch.width == roi_out->width
                                    && g->process_patch.height == roi_out->height);
      const float *layer_pixels = g->process_patch.pixels;
      drawlayer_process_scratch_t *scratch = NULL;
      if(!direct_copy)
      {
        const gint64 resample_t0 = g_get_monotonic_time();
        scratch = _get_process_scratch();
        if(!scratch)
        {
          _copy_input_to_output(input, output, roi_out->width, roi_out->height);
          return 0;
        }

        float *layerbuf = _ensure_process_scratch_buffer(&scratch->layerbuf, &scratch->layerbuf_pixels, pixels);
        if(!layerbuf)
        {
          _copy_input_to_output(input, output, roi_out->width, roi_out->height);
          return 0;
        }
        const dt_iop_roi_t source_process_roi = {
          .x = 0,
          .y = 0,
          .width = g->process_patch.width,
          .height = g->process_patch.height,
          .scale = 1.0f,
        };
        const dt_iop_roi_t target_roi = {
          .x = 0,
          .y = 0,
          .width = roi_out->width,
          .height = roi_out->height,
          .scale = 1.0f,
        };
        dt_iop_clip_and_zoom(layerbuf, g->process_patch.pixels, &target_roi, &source_process_roi,
                             roi_out->width, g->process_patch.width);
        layer_pixels = layerbuf;
        {
          const gint64 now = g_get_monotonic_time();
          dt_print(DT_DEBUG_PERF, "[drawlayer] process step=resample-process-patch ms=%.3f",
                   (now - resample_t0) / 1000.0);
        }
      }

      _blend_layer_over_input(output, input, layer_pixels, pixels, use_preview_bg, preview_bg);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=blend-process-patch ms=%.3f total=%.3f",
                 (now - process_t) / 1000.0, (now - process_t0) / 1000.0);
      }
      return 0;
    }
  }

  if(data && data->headless_cache_valid && data->headless_base_patch.pixels
     && data->headless_cache_imgid == piece->pipe->image.id)
  {
    /* Headless / non-GUI pipelines keep their own authoritative cache loaded in
     * `commit_params()`, outside of `process()`. They still use the same affine
     * transform logic, but without any GUI-owned transformed cache. */
    const dt_iop_roi_t process_roi = roi_in ? *roi_in : *roi_out;
    const dt_iop_roi_t source_full_roi = {
      .x = 0,
      .y = 0,
      .width = data->headless_base_patch.width,
      .height = data->headless_base_patch.height,
      .scale = 1.0f,
    };
    drawlayer_process_scratch_t scratch = { 0 };
    float *layerbuf = _ensure_process_scratch_buffer(&scratch.layerbuf, &scratch.layerbuf_pixels, pixels);
    if(layerbuf)
    {
      dt_iop_roi_t combined_roi = { 0 };
      _build_combined_process_roi(piece, &process_roi, current_full_w, current_full_h,
                                  data->headless_base_patch.width, data->headless_base_patch.height, &combined_roi);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-combined-roi-headless ms=%.3f",
                 (now - process_t) / 1000.0);
        process_t = now;
      }
      _patch_rdlock(&data->headless_base_patch);
      dt_iop_clip_and_zoom(layerbuf, data->headless_base_patch.pixels, &combined_roi, &source_full_roi,
                           roi_out->width, data->headless_base_patch.width);
      _patch_rdunlock(&data->headless_base_patch);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=clip-and-zoom-headless ms=%.3f",
                 (now - process_t) / 1000.0);
        process_t = now;
      }
      _blend_layer_over_input(output, input, layerbuf, pixels, use_preview_bg, preview_bg);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=blend-headless ms=%.3f total=%.3f",
                 (now - process_t) / 1000.0, (now - process_t0) / 1000.0);
      }
      _free_tracked_temp_buffer((void **)&scratch.layerbuf, "drawlayer process scratch");
      return 0;
    }
  }

  /* The sidecar is intentionally managed outside of `process()`. Once a layer
   * is loaded, the in-memory caches are authoritative until module-level flush
   * points write them back. If we do not have a usable in-memory cache here,
   * the correct backend behavior is therefore a no-op pass-through rather than
   * reopening/scanning/loading the TIFF in the hot process path. */
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=no-cache-pass-through ms=%.3f total=%.3f",
             (now - process_t) / 1000.0, (now - process_t0) / 1000.0);
  }
  _copy_input_to_output(input, output, roi_out->width, roi_out->height);
  return 0;
}
