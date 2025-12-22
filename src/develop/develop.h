/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <cairo.h>
#include <glib.h>
#include <inttypes.h>
#include <stdint.h>

#include "common/debug.h"
#include "common/darktable.h"
#include "common/dtpthread.h"
#include "common/image.h"
#include "control/settings.h"
#include "develop/imageop.h"
#include "develop/dev_history.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dt_iop_module_t;

typedef enum dt_dev_overexposed_colorscheme_t
{
  DT_DEV_OVEREXPOSED_BLACKWHITE = 0,
  DT_DEV_OVEREXPOSED_REDBLUE = 1,
  DT_DEV_OVEREXPOSED_PURPLEGREEN = 2
} dt_dev_overexposed_colorscheme_t;

typedef enum dt_dev_overlay_colors_t
{
  DT_DEV_OVERLAY_GRAY = 0,
  DT_DEV_OVERLAY_RED = 1,
  DT_DEV_OVERLAY_GREEN = 2,
  DT_DEV_OVERLAY_YELLOW = 3,
  DT_DEV_OVERLAY_CYAN = 4,
  DT_DEV_OVERLAY_MAGENTA = 5
} dt_dev_overlay_colors_t;

typedef enum dt_dev_rawoverexposed_mode_t {
  DT_DEV_RAWOVEREXPOSED_MODE_MARK_CFA = 0,
  DT_DEV_RAWOVEREXPOSED_MODE_MARK_SOLID = 1,
  DT_DEV_RAWOVEREXPOSED_MODE_FALSECOLOR = 2,
} dt_dev_rawoverexposed_mode_t;

typedef enum dt_dev_rawoverexposed_colorscheme_t {
  DT_DEV_RAWOVEREXPOSED_RED = 0,
  DT_DEV_RAWOVEREXPOSED_GREEN = 1,
  DT_DEV_RAWOVEREXPOSED_BLUE = 2,
  DT_DEV_RAWOVEREXPOSED_BLACK = 3
} dt_dev_rawoverexposed_colorscheme_t;

typedef enum dt_dev_transform_direction_t
{
  DT_DEV_TRANSFORM_DIR_ALL = 0,
  DT_DEV_TRANSFORM_DIR_FORW_INCL = 1,
  DT_DEV_TRANSFORM_DIR_FORW_EXCL = 2,
  DT_DEV_TRANSFORM_DIR_BACK_INCL = 3,
  DT_DEV_TRANSFORM_DIR_BACK_EXCL = 4
} dt_dev_transform_direction_t;

typedef enum dt_dev_pixelpipe_display_mask_t
{
  DT_DEV_PIXELPIPE_DISPLAY_NONE = 0,
  DT_DEV_PIXELPIPE_DISPLAY_MASK = 1 << 0,
  DT_DEV_PIXELPIPE_DISPLAY_CHANNEL = 1 << 1,
  DT_DEV_PIXELPIPE_DISPLAY_OUTPUT = 1 << 2,
  DT_DEV_PIXELPIPE_DISPLAY_L = 1 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_a = 2 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_b = 3 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_R = 4 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_G = 5 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_B = 6 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_GRAY = 7 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_LCH_C = 8 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_LCH_h = 9 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_H = 10 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_S = 11 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_l = 12 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz = 13 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz = 14 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz = 15 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU = 16 << 3, // show module's output without processing by later iops
  DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU_MONO = 17 << 3, // same as above but specific for pre-demosaic to stay monochrome
  DT_DEV_PIXELPIPE_DISPLAY_ANY = 0xff << 2,
  DT_DEV_PIXELPIPE_DISPLAY_STICKY = 1 << 16
} dt_dev_pixelpipe_display_mask_t;

typedef enum dt_develop_detail_mmask_t
{
  DT_DEV_DETAIL_MASK_NONE = 0,
  DT_DEV_DETAIL_MASK_REQUIRED = 1,
  DT_DEV_DETAIL_MASK_DEMOSAIC = 2,
  DT_DEV_DETAIL_MASK_RAWPREPARE = 4
} dt_develop_detail_mask_t;

typedef enum dt_clipping_preview_mode_t
{
  DT_CLIPPING_PREVIEW_GAMUT = 0,
  DT_CLIPPING_PREVIEW_ANYRGB = 1,
  DT_CLIPPING_PREVIEW_LUMINANCE = 2,
  DT_CLIPPING_PREVIEW_SATURATION = 3
} dt_clipping_preview_mode_t;

typedef enum dt_dev_pipe_type_t
{
  DT_DEV_PIPE = 0,
  DT_DEV_PREVIEW_PIPE = 1
} dt_dev_pipe_type_t;
typedef struct dt_dev_proxy_exposure_t
{
  struct dt_iop_module_t *module;
  float (*get_exposure)(struct dt_iop_module_t *exp);
  float (*get_black)(struct dt_iop_module_t *exp);
} dt_dev_proxy_exposure_t;

struct dt_dev_pixelpipe_t;

typedef struct dt_backbuf_t
{
  void *buffer;         // image data
  size_t width;          // pixel size of image
  size_t height;         // pixel size of image
  uint64_t hash;         // checksum/integrity hash, for example to connect to a cacheline
  const char *op;        // name of the backbuf
  size_t bpp;            // bits per pixels
} dt_backbuf_t;


typedef struct dt_develop_t
{
  // != 0 if the gui should be notified of changes in hist stack and modules should be
  // gui_init'ed.
  int32_t gui_attached; 

  int exit; // set to 1 to close background darkroom pipeline threads
  uint32_t average_delay;
  uint32_t preview_average_delay;
  struct dt_iop_module_t *gui_module; // this module claims gui expose/event callbacks.

  // darkroom border size: ISO 12646 borders or user-defined borders
  int32_t border_size;

  // Those are the darkroom main widget size, aka max paintable area.
  // This size is allocated by Gtk from the window size minus all panels.
  // It is NOT the size of the backbuffer/ROI.
  int32_t orig_width, orig_height;

  // width = orig_width - 2 * border_size,
  // height = orig_height - 2 * border_size,
  // aka the surface actually covered by an image backbuffer (ROI)
  // set by `dt_dev_configure()`
  int32_t width, height;

  // natural scaling = MIN(dev->width / dev->pipe->processed_width, dev->height / dev->pipe->processed_height)
  // aka ensure that image fits into widget minus margins/borders.
  float natural_scale;

  // User-defined scaling factor, related to GUI zoom.
  // Applies on top of natural scale
  float scaling;

  // Relative coordinates of the center of the ROI, expressed with
  // regard to the complete image.
  float x, y;

  // image processing pipeline with caching
  struct dt_dev_pixelpipe_t *pipe, *preview_pipe;

  // image under consideration, which
  // is copied each time an image is changed. this means we have some information
  // always cached (might be out of sync, so stars are not reliable), but for the iops
  // it's quite a convenience to access trivial stuff which is constant anyways without
  // calling into the cache explicitly. this should never be accessed directly, but
  // by the iop through the copy their respective pixelpipe holds, for thread-safety.
  dt_image_t image_storage;

  // history stack
  dt_pthread_mutex_t history_mutex;
  int32_t history_end;
  GList *history;

  // operations pipeline
  int32_t iop_instance;
  GList *iop;
  // iop's to be deleted
  GList *alliop;

  // iop order
  int iop_order_version;
  GList *iop_order_list;

  // profiles info
  GList *allprofile_info;

  // histogram for display.
  uint32_t *histogram_pre_tonecurve, *histogram_pre_levels;
  uint32_t histogram_pre_tonecurve_max, histogram_pre_levels_max;

  // list of forms iop can use for masks or whatever
  GList *forms;
  // integrity hash of forms
  uint64_t forms_hash;
  // forms have been added or removed or changed and need to be committed to history
  gboolean forms_changed;
  struct dt_masks_form_t *form_visible;
  struct dt_masks_form_gui_t *form_gui;
  // all forms to be linked here for cleanup:
  GList *allforms;

  dt_backbuf_t raw_histogram; // backbuf to prepare the raw histogram (before white balance)
  dt_backbuf_t output_histogram;  // backbuf to prepare the display-agnostic output histogram (in the middle of colorout)
  dt_backbuf_t display_histogram; // backbuf to prepare the display-referred output histogram (at the far end of the pipe)

  // if dev is GUI-attached, auto save history 15s after the last change is made.
  guint auto_save_timeout;

  // Track history changes from C. Note: we have a DB variant.
  uint64_t history_hash;

  /* proxy for communication between plugins and develop/darkroom */
  struct
  {
    // list of exposure iop instances, with plugin hooks, used by histogram dragging functions
    // each element is dt_dev_proxy_exposure_t
    dt_dev_proxy_exposure_t exposure;

    // modulegroups plugin hooks
    struct
    {
      struct dt_lib_module_t *module;
      /* switch module group */
      void (*set)(struct dt_lib_module_t *self, uint32_t group);
      /* get current module group */
      uint32_t (*get)(struct dt_lib_module_t *self);
      /* get activated module group */
      uint32_t (*get_activated)(struct dt_lib_module_t *self);
      /* test if iop group flags matches modulegroup */
      gboolean (*test)(struct dt_lib_module_t *self, uint32_t group, uint32_t iop_group);
      /* switch to modulegroup */
      void (*switch_group)(struct dt_lib_module_t *self, struct dt_iop_module_t *module);
      /* update modulegroup visibility */
      void (*update_visibility)(struct dt_lib_module_t *self);
      /* test if module is preset in one of the current groups */
      gboolean (*test_visible)(struct dt_lib_module_t *self, gchar *module);
    } modulegroups;

    // snapshots plugin hooks
    struct
    {
      // this flag is set by snapshot plugin to signal that expose of darkroom
      // should store cairo surface as snapshot to disk using filename.
      gboolean request;
      const gchar *filename;
    } snapshot;

    // masks plugin hooks
    struct
    {
      struct dt_lib_module_t *module;
      /* treview list refresh */
      void (*list_change)(struct dt_lib_module_t *self);
      void (*list_remove)(struct dt_lib_module_t *self, int formid, int parentid);
      void (*list_update)(struct dt_lib_module_t *self);
      /* selected forms change */
      void (*selection_change)(struct dt_lib_module_t *self, struct dt_iop_module_t *module, const int selectid, const int throw_event);
    } masks;

    // what is the ID of the module currently doing pipeline chromatic adaptation ?
    // this is to prevent multiple modules/instances from doing white balance globally.
    // only used to display warnings in GUI of modules that should probably not be doing white balance
    struct dt_iop_module_t *chroma_adaptation;

    // is the WB module using D65 illuminant and not doing full chromatic adaptation ?
    gboolean wb_is_D65;
    dt_aligned_pixel_t wb_coeffs;

  } proxy;

  // for the overexposure indicator
  struct
  {
    GtkWidget *floating_window, *button; // yes, having gtk stuff in here is ugly. live with it.

    gboolean enabled;
    dt_dev_overexposed_colorscheme_t colorscheme;
    float lower;
    float upper;
    dt_clipping_preview_mode_t mode;
  } overexposed;

  // for the raw overexposure indicator
  struct
  {
    GtkWidget *floating_window, *button; // yes, having gtk stuff in here is ugly. live with it.

    gboolean enabled;
    dt_dev_rawoverexposed_mode_t mode;
    dt_dev_rawoverexposed_colorscheme_t colorscheme;
    float threshold;
  } rawoverexposed;

  struct
  {
    GtkWidget *floating_window, *button; // 10 years later, still ugly

    float brightness;
    int border;
  } display;

  // ISO 12646-compliant colour assessment conditions
  struct
  {
    GtkWidget *button; // yes, ugliness is the norm. what did you expect ?
    gboolean enabled;
  } iso_12646;

  // the display profile related things (softproof, gamut check, profiles ...)
  struct
  {
    GtkWidget *floating_window, *softproof_button, *gamut_button;
  } profile;

  int mask_form_selected_id; // select a mask inside an iop
  gboolean darkroom_skip_mouse_events; // skip mouse events for masks
  gboolean mask_lock;
  gint drawing_timeout;
} dt_develop_t;

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached);
void dt_dev_cleanup(dt_develop_t *dev);

void dt_dev_process_image_job(dt_develop_t *dev);
void dt_dev_process_preview_job(dt_develop_t *dev);
// launch jobs above
void dt_dev_process_image(dt_develop_t *dev);
void dt_dev_process_preview(dt_develop_t *dev);

// Lazy helpers that will update GUI pipelines (main image and small preview)
// only when needed, and only the one(s) needed.
void dt_dev_refresh_ui_images_real(dt_develop_t *dev);
#define dt_dev_refresh_ui_images(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_refresh_ui_images_real, (dev))

int dt_dev_load_image(dt_develop_t *dev, const int32_t imgid);
/** checks if provided imgid is the image currently in develop */
int dt_dev_is_current_image(dt_develop_t *dev, int32_t imgid);


// Force a full rebuild of the pipe, needed when module order is changed.
// Resync the full history, which may be expensive.
// Pixelpipe cache will need to be flushed too when this is called,
// for raster masks to work properly.
void dt_dev_pixelpipe_rebuild(struct dt_develop_t *dev);

void dt_dev_invalidate_real(dt_develop_t *dev);
// Invalidate the main image preview in darkroom, resync only the last history item.
// This is the most common usecase when interacting with modules and masks.
#define dt_dev_invalidate(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_invalidate_real, (dev))

void dt_dev_invalidate_preview_real(dt_develop_t *dev);
// Invalidate the thumbnail preview in darkroom, resync only the last history item.
#define dt_dev_invalidate_preview(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_invalidate_preview_real, (dev))

void dt_dev_invalidate_all_real(dt_develop_t *dev);
// Invalidate the main image and the thumbnail in darkroom, resync only the last history item.
#define dt_dev_invalidate_all(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_invalidate_all_real, (dev))

void dt_dev_invalidate_zoom_real(dt_develop_t *dev);
// Invalidate the main image preview in darkroom.
// This doesn't resync history at all, only update the coordinates of the region of interest (ROI).
#define dt_dev_invalidate_zoom(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_invalidate_zoom_real, (dev))

void dt_dev_invalidate_zoom_preview(dt_develop_t *dev);

// Invalidate the main image and the thumbnail in darkroom.
// Resync the whole history, which may be expensive.
void dt_dev_pixelpipe_resync_all(dt_develop_t *dev);

// Invalidate the main image in darkroom.
// Resync the whole history, which may be expensive.
void dt_dev_pixelpipe_resync_main(dt_develop_t *dev);

void dt_dev_pixelpipe_resync_preview(dt_develop_t *dev);

// Flush caches of dev pipes and force a full recompute
void dt_dev_reprocess_all(dt_develop_t *dev);

void dt_dev_get_processed_size(const dt_develop_t *dev, int *procw, int *proch);

float dt_dev_get_zoom_scale(dt_develop_t *dev, gboolean preview);

/**
 * @brief Get the pointer position from widget space to preview buffer space [0..1].
 * 
 * NOTE: The input mouse coordinates are without border subtraction.
 * 
 * @param dev the develop instance
 * @param px the x mouse coordinate in widget space, with no border subtraction.
 * @param py the y mouse coordinate in widget space, with no border subtraction.
 * @param mouse_x the returned x mouse coordinate relative to processed image [0..1].
 * @param mouse_y the returned y mouse coordinate relative to processed image [0..1].
 */
void dt_dev_retrieve_full_pos(dt_develop_t *dev, const int px, const int py, float *mouse_x, float *mouse_y);

void dt_dev_configure_real(dt_develop_t *dev, int wd, int ht);
#define dt_dev_configure(dev, wd, ht) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_configure_real, (dev), (wd), (ht))

/**
 * @brief Ensure that the current ROI position is within allowed bounds .
 * 
 * @param dev the develop instance
 * @param dev_x the normalized x position of ROI
 * @param dev_y the normalized y position of ROI
 * @param box_w the width of navigation's box
 * @param box_h the height of navigation's box
 */
void dt_dev_check_zoom_pos_bounds(dt_develop_t *dev, float *dev_x, float *dev_y, float *box_w, float *box_h);

/*
 * exposure plugin hook, set the exposure and the black level
 */

/** get exposure */
float dt_dev_exposure_get_exposure(dt_develop_t *dev);
/** get exposure black level */
float dt_dev_exposure_get_black(dt_develop_t *dev);

/*
 * modulegroups plugin hooks
 */
/** switch to modulegroup of module */
void dt_dev_modulegroups_switch(dt_develop_t *dev, struct dt_iop_module_t *module);
/** update modulegroup visibility */
void dt_dev_modulegroups_update_visibility(dt_develop_t *dev);
/** set the active modulegroup */
void dt_dev_modulegroups_set(dt_develop_t *dev, uint32_t group);
/** get the active modulegroup */
uint32_t dt_dev_modulegroups_get(dt_develop_t *dev);
/** reorder the module list */
void dt_dev_reorder_gui_module_list(dt_develop_t *dev);

/** request snapshot */
void dt_dev_snapshot_request(dt_develop_t *dev, const char *filename);

/** update gliding average for pixelpipe delay */
void dt_dev_average_delay_update(const dt_times_t *start, uint32_t *average_delay);

/*
 * masks plugin hooks
 */
void dt_dev_masks_list_change(dt_develop_t *dev);
void dt_dev_masks_list_update(dt_develop_t *dev);
void dt_dev_masks_list_remove(dt_develop_t *dev, int formid, int parentid);
void dt_dev_masks_selection_change(dt_develop_t *dev, struct dt_iop_module_t *module, const int selectid, const int throw_event);

/** integrity hash of the forms/shapes stack */
void dt_dev_masks_update_hash(dt_develop_t *dev);

/*
 * multi instances
 */
/** duplicate a existent module */
struct dt_iop_module_t *dt_dev_module_duplicate(dt_develop_t *dev, struct dt_iop_module_t *base);
/** remove an existent module */
void dt_dev_module_remove(dt_develop_t *dev, struct dt_iop_module_t *module);
/** same, but for all modules */
void dt_dev_modules_update_multishow(dt_develop_t *dev);

/** generates item multi-instance name without mnemonics */
gchar *dt_history_item_get_name(const struct dt_iop_module_t *module);
gchar *dt_history_item_get_name_html(const struct dt_iop_module_t *module);

/** generate item multi-instance name with mnemonics, for Gtk labels */
gchar *dt_history_item_get_label(const struct dt_iop_module_t *module);


/*
 * distort functions
 */
/** apply all transforms to the specified points (in preview pipe space) */
int dt_dev_distort_transform(dt_develop_t *dev, float *points, size_t points_count);
/** reverse apply all transforms to the specified points (in preview pipe space) */
int dt_dev_distort_backtransform(dt_develop_t *dev, float *points, size_t points_count);
/** same fct, but we can specify iop with priority between pmin and pmax */
int dt_dev_distort_transform_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                  float *points, size_t points_count);
/** same fct, but can only be called from a distort_transform function called by dt_dev_distort_transform_plus */
int dt_dev_distort_transform_locked(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order,
                                    const int transf_direction, float *points, size_t points_count);
/** same fct as dt_dev_distort_backtransform, but we can specify iop with priority between pmin and pmax */
int dt_dev_distort_backtransform_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                      float *points, size_t points_count);
/** same fct, but can only be called from a distort_backtransform function called by dt_dev_distort_backtransform_plus */
int dt_dev_distort_backtransform_locked(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order,
                                    const int transf_direction, float *points, size_t points_count);

/** get the iop_pixelpipe instance corresponding to the iop in the given pipe */
struct dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
                                                           struct dt_iop_module_t *module);
/*
 * hash functions
 */
/** Get the global hash of the last module in pipe */
uint64_t dt_dev_hash(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe);;
/** wait until hash value found in hash matches hash value defined by dev/pipe/pmin/pmax with timeout */
int dt_dev_wait_hash(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction, dt_pthread_mutex_t *lock,
                     const volatile uint64_t *const hash);
/** synchronize pixelpipe by means hash values by waiting with timeout and potential reprocessing
 * FIXME: modules that need to resync internal data with pipeline should listen to the PREVIEW_PIPE_RECOMPUTED signal.
 * This function relies on timeouts, waiting for the pipe to finish, and the exception is not caught if the
 * timeout expires with no result.
*/
int dt_dev_sync_pixelpipe_hash(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction, dt_pthread_mutex_t *lock,
                               const volatile uint64_t *const hash);

/*
 *   history undo support helpers for darkroom
 */

/* all history change must be enclosed into a start / end call */
void dt_dev_undo_start_record(dt_develop_t *dev);
void dt_dev_undo_end_record(dt_develop_t *dev);

// Getter and setter for global mask lock (GUI)
// Can be overriden by key accels

gboolean dt_masks_get_lock_mode(dt_develop_t *dev);
void dt_masks_set_lock_mode(dt_develop_t *dev, gboolean mode);

// Count all the mask forms used x history entries, up to a certain threshold.
// Stop counting when the threshold is reached, for performance.
guint dt_dev_mask_history_overload(GList *dev_history, guint threshold);

// Write the `darktable|changed` tag on the current picture upon history modification
void dt_dev_append_changed_tag(const int32_t imgid);

/**
 * @brief Compute the theoritical final size of a pipeline taking the full-resolution image at input.
 *
 * Note: this creates a dummy pipeline and develop but doesn't trigger file I/O.
 *
 * @param dev An inited develop object, with `dev->iop` and `dev->history` already populated. Can be NULL, in which case a temporary develop is created from scratch (slower).
 * @param pipe An inited pipeline object, with `pipe->iwidth` and `pipe->iheight` already set and pipeline pieces params already synchronized.
 * Can be NULL, in which case a temporary pipeline is created from scratch (slower).
 * @param imgid
 * @param input_width Not used if `pipe` is not NULL, will use `pipe->iwidth`
 * @param input_height Not used if `pipe` is not NULL, will use `pipe->iheight`
 * @param processed_width returned computed value
 * @param processed_height returned computed value
 */
void dt_dev_get_final_size(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const int32_t imgid, const int input_width, const int input_height, int *processed_width, int *processed_height);


// This needs to run after `dt_dev_pixelpipe_get_roi_out()` so `pipe->processed_width`
// and `pipe->processed_height` are defined.
// Natural scale is the rescaling factor such that the full-res pipeline output
// (real or virtual) fits within darkroom widget area (minus borders/margins)
float dt_dev_get_natural_scale(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe);
// This needs to run after `dt_dev_pixelpipe_get_roi_out()` so `pipe->processed_width`
// and `pipe->processed_height` are defined.
// Natural scale is the rescaling factor such that the full-res preview pipeline output
// (real or virtual) fits within darkroom widget area (minus borders/margins)
// this is like dt_dev_get_natural_scale() but for preview pipe based on its output processed size
float dt_dev_get_preview_natural_scale(dt_develop_t *dev);
// Get the current zoom factor ( scaling * natural_scale )
float dt_dev_get_zoom_level(const dt_develop_t *dev);

// Reset darkroom ROI scaling and position
void dt_dev_reset_roi(dt_develop_t *dev);

/**
 * @brief Clip the view to the ROI.
 * WARNING: this must be done before any translation.
 * 
 * @param dev the develop instance
 * @param cr the cairo context to clip on
 * @param width the view width
 * @param height the view height
 * @return gboolean TRUE if the image dimension are 0x0
 */
gboolean dt_dev_clip_roi(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height);

/**
 * @brief Scale the ROI to fit within given width/height, centered.
 * 
 * @param dev the develop instance
 * @param cr the cairo context to draw on
 * @param width the widget width
 * @param height the widget height
 * @return gboolean TRUE if the image dimension are 0x0
 */
gboolean dt_dev_rescale_roi(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height);

/**
 * @brief Scale the ROI to fit the input size within given width/height, centered.
 * 
 * @param dev the develop instance
 * @param cr the cairo context to draw on
 * @param width the widget width
 * @param height the widget height
 * @return gboolean TRUE if the image dimension are 0x0
 */
gboolean dt_dev_rescale_roi_to_input(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height);

/**
 * @brief Ensure that the current zoom level is within allowed bounds (for scrolling).
 * 
 * @param dev the develop instance
 * @return gboolean TRUE if the zoom level was adjusted, FALSE otherwise
 */
gboolean dt_dev_check_zoom_scale_bounds(dt_develop_t *dev);

/**
 * @brief Get point in input image space from point in normalized ROI space.
 * The function performs a distortion backtransform.
 * 
 * @param dev the develop instance
 * @param normalize_out if TRUE, out_x and out_y will be normalized [0..1], if FALSE, they will be in pixel coordinates.
 * @param in_x the x point in normalized ROI space [0..1].
 * @param in_y the y point in normalized ROI space [0..1].
 * @param out_x the returned x point in input image space, normalized or pixel depending on `normalize_out`.
 * @param out_y the returned y point in input image space, normalized or pixel depending on `normalize_out`.
 * @return gboolean TRUE on success, FALSE if image has 0 dimension.
 */
gboolean dt_dev_roi_to_input_space(dt_develop_t *dev, /*gboolean normalized_in,*/ gboolean normalize_out, const float in_x, const float in_y, float *out_x, float *out_y);

/**
 * @brief Convert a delta vector in ROI space to input image space.
 * The function performs a distortion backtransform.
 * 
 * @param dev the develop instance
 * @param delta the delta vector in anormalized ROI space.
 * @param in the input point in normalized ROI space [0..1].
 * @param points the returned two points in input image space, normalized or pixel depending on `normalize_out`.

 * @return gboolean TRUE on success, FALSE if image has 0 dimension or fails.
 */
gboolean dt_dev_roi_delta_to_input_space(dt_develop_t *dev, const float delta[2],/* gboolean normalize_out,*/ const float in[2], float points[2]);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
