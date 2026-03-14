/*
    This file is part of darktable,
    Copyright (C) 2009-2014 johannes hanika.
    Copyright (C) 2010 Bruce Guenter.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2012-2014, 2020-2021 Aldric Renaudin.
    Copyright (C) 2012, 2016, 2019-2021 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2012, 2014, 2016-2017, 2019 Ulrich Pegelow.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2014-2016, 2020 Roman Lebedev.
    Copyright (C) 2016 Alexander V. Smal.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2017, 2019, 2021 luzpaz.
    Copyright (C) 2019-2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2020 Dan Torop.
    Copyright (C) 2020-2021 Diederik Ter Rahe.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020-2022 Hanno Schwalm.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023, 2025 Alynx Zhou.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2025-2026 Guillaume Stutin.
    
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
#include <stddef.h>
#include <stdint.h>

#include "common/debug.h"
#include "common/darktable.h"
#include "common/dtpthread.h"
#include "common/image.h"
#include "control/settings.h"
#include "develop/imageop.h"
#include "develop/dev_history.h"
#include "develop/dev_pixelpipe.h"


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

struct dt_dev_pixelpipe_t;

typedef struct dt_develop_t
{
  // != 0 if the gui should be notified of changes in hist stack and modules should be
  // gui_init'ed.
  int32_t gui_attached; 

  int exit; // set to 1 to close background darkroom pipeline threads
  struct dt_iop_module_t *gui_module; // this module claims gui expose/event callbacks.

  // The roi structure is used in darkroom GUI only.
  // It defines the output size of the image backbuffer fitting
  // into the darkroom center widget. This is critically used for all
  // GUI <-> RAW pixel coordinates conversions. It should be recomputed
  // ASAP when widget size changes.
  struct {
    // width = orig_width - 2 * border_size,
    // height = orig_height - 2 * border_size,
    // aka the surface actually covered by an image backbuffer (ROI)
    // set by `dt_dev_configure()`
    int32_t width, height;

    // User-defined scaling factor, related to GUI zoom.
    // Applies on top of natural scale
    float scaling;

    // Relative coordinates of the center of the ROI, expressed with
    // regard to the complete image.
    float x, y;

    // darkroom border size: ISO 12646 borders or user-defined borders
    int32_t border_size;

    // Those are the darkroom main widget size, aka max paintable area.
    // This size is allocated by Gtk from the window size minus all panels.
    // It is NOT the size of the backbuffer/ROI.
    int32_t orig_width, orig_height;

    // Dimensions of the preview backbuffer, depending on the 
    // darkroom main widget size and DPI factor.
    // These are computed early, before we have the actual buffer.
    // Use them everywhere in GUI.
    // They respect the final image aspect ratio and fit within
    // the width x height bounding box.
    int32_t preview_width, preview_height;

    // Dimension of the main image backbuffer
    // They are at lower than or equal to (width, height),
    // the bounding box defined by the widget where main image fits.
    // Since the ROI may clip the zoomed-in image, they don't respect
    // the final image aspect ratio
    int32_t main_width, main_height;

    // natural scaling = MIN(dev->width / dev->roi.processed_width, dev->height / dev->roi.processed_height)
    // aka ensure that image fits into widget minus margins/borders.
    float natural_scale;

    // Dimensions of the full-resolution RAW image
    // being worked on.
    int32_t raw_width, raw_height;

    // Dimensions of the final processed image if we processed it full-resolution.
    // This is used to get the final aspect ratio of an image,
    // taking all cropping and distortions into account.
    int32_t processed_width, processed_height;

    // Conveniency state to check if all widget sizes are inited
    gboolean gui_inited;

    // Conveniency state to check if input (raw image) sizes are inited
    gboolean raw_inited;

    // Conveniency state to check if all output (backbuffer) sizes are inited
    gboolean output_inited;

  } roi;

  // image processing pipeline with caching
  struct dt_dev_pixelpipe_t *pipe, *preview_pipe;
  // Virtual preview-like pipeline used for geometry/ROI computations on the GUI thread.
  // It mirrors preview_pipe history/nodes but never processes pixels.
  // It is meant for fast access from the GUI mainthread without waiting for threads to return.
  struct dt_dev_pixelpipe_t *virtual_pipe;

  // image under consideration, which
  // is copied each time an image is changed. this means we have some information
  // always cached (might be out of sync, so stars are not reliable), but for the iops
  // it's quite a convenience to access trivial stuff which is constant anyways without
  // calling into the cache explicitly. this should never be accessed directly, but
  // by the iop through the copy their respective pixelpipe holds, for thread-safety.
  dt_image_t image_storage;

  // Protect read & write to dev->history and dev->forms
  // and other related stuff like history_end, hashes, etc.
  dt_pthread_rwlock_t history_mutex;

  // We don't always apply the full history to modules,
  // this is the cursor where we stop the list.
  // Note: history_end = number of history items,
  // since we consider the 0th element to be the RAW image
  // (no IOP, no history entry, no item on the GList). 
  // So the index of the history
  // entry matching history_end is history_end - 1,
  int32_t history_end;

  // history stack
  GList *history;

  // operations pipeline
  int32_t iop_instance;
  GList *iop;
  // iop's to be deleted
  GList *alliop;

  // iop order
  int iop_order_version;
  GList *iop_order_list;

  // Undo tracking for history changes. This is managed by dt_dev_undo_start_record()
  // / dt_dev_undo_end_record() and stores "before" snapshots until the outermost
  // change completes.
  int undo_history_depth;
  GList *undo_history_before_snapshot;
  int undo_history_before_end;
  GList *undo_history_before_iop_order_list;

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
  struct dt_masks_form_gui_t *form_gui;
  // all forms to be linked here for cleanup:
  GList *allforms;

  // Mutex lock protecting masks, shapes, etc.
  // aka dev->forms and dev->all_forms
  dt_pthread_rwlock_t masks_mutex;

  dt_backbuf_t raw_histogram;     // backbuf to prepare the raw histogram (before white balance)
  dt_backbuf_t output_histogram;  // backbuf to prepare the display-agnostic output histogram (in the middle of colorout)
  dt_backbuf_t display_histogram; // backbuf to prepare the display-referred output histogram (at the far end of the pipe)
  
  // Track history changes from C.
  // This is updated when history is changed, read or written.
  dt_atomic_uint64 history_hash;

  // Darkroom pipelines are running fulltime in background until leaving darkroom.
  // Set that to TRUE once they get shutdown.
  gboolean pipelines_started;

  /* proxy for communication between plugins and develop/darkroom */
  struct
  {
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

  // progress bar
  struct 
  {
    int total, completed;
  } progress;

  gboolean darkroom_skip_mouse_events; // skip mouse events for masks
  gboolean mask_lock;

  cairo_surface_t *image_surface;

  gboolean loading_cache;
} dt_develop_t;

static inline uint64_t dt_dev_get_history_hash(const dt_develop_t *dev)
{
  return dt_atomic_get_uint64(&dev->history_hash);
}

static inline void dt_dev_set_history_hash(dt_develop_t *dev, const uint64_t history_hash)
{
  dt_atomic_set_uint64(&dev->history_hash, history_hash);
}

#ifdef __cplusplus
extern "C" {
#endif

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached);
void dt_dev_cleanup(dt_develop_t *dev);
GList *dt_dev_load_modules(dt_develop_t *dev);

void dt_dev_process_image_job(dt_develop_t *dev);
void dt_dev_process_preview_job(dt_develop_t *dev);

typedef enum dt_dev_image_storage_t
{
  DT_DEV_IMAGE_STORAGE_OK = 0,
  DT_DEV_IMAGE_STORAGE_MIPMAP_NOT_FOUND = 1,
  DT_DEV_IMAGE_STORAGE_DB_NOT_READ = 2,
} dt_dev_image_storage_t;

// Optionally prefetch `DT_MIPMAP_FULL`, then refresh dev->image_storage from the image cache.
// Returns a status code to differentiate missing source image data from DB/cache failures.
dt_dev_image_storage_t dt_dev_ensure_image_storage(dt_develop_t *dev, const int32_t imgid);

// GUI-thread second phase of image loading: read history and rebuild pipelines.
int dt_dev_load_image_finish(dt_develop_t *dev, const int32_t imgid);

// Start background pipeline threads. They run fulltime until we close darkroom,
// so no need to recall that
void dt_dev_start_all_pipelines(dt_develop_t *dev);

int dt_dev_load_image(dt_develop_t *dev, const int32_t imgid);
/** checks if provided imgid is the image currently in develop */
int dt_dev_is_current_image(dt_develop_t *dev, int32_t imgid);

void dt_dev_get_processed_size(const dt_develop_t *dev, int *procw, int *proch);

float dt_dev_get_zoom_scale(const dt_develop_t *dev, gboolean preview);

// Set all the params of a backbuffer at once
void dt_dev_set_backbuf(dt_backbuf_t *backbuf, const int width, const int height, const size_t bpp, 
                        const int64_t hash, const int64_t history_hash);

/**
 * @brief Coordinate conversion helpers between widget, normalized image, and absolute image spaces.
 * 
 * Widget space is assumed to be the darkroom center view and doesn't account for borders, zooming, panning, etc.
 * RAW space is the full-resolution input fed to the pipeline.
 * Image space is the output image resulting from applying a full history over the full-resolution input.
 * Preview space is the downscaled output image preview as displayed in darkroom.
 * 
 * @param dev develop instance
 * @param points pointer to num_points coordinate pairs stored as {x, y}; data is modified in place.
 * @param num_points number of coordinate pairs referenced by points.
 */
void dt_dev_coordinates_widget_to_image_norm(dt_develop_t *dev, float *points, size_t num_points);
void dt_dev_coordinates_image_norm_to_widget(dt_develop_t *dev, float *points, size_t num_points);
void dt_dev_coordinates_image_norm_to_image_abs(dt_develop_t *dev, float *points, size_t num_points);
void dt_dev_coordinates_image_abs_to_image_norm(dt_develop_t *dev, float *points, size_t num_points);
void dt_dev_coordinates_raw_abs_to_raw_norm(dt_develop_t *dev, float *points, size_t num_points);
void dt_dev_coordinates_raw_norm_to_raw_abs(dt_develop_t *dev, float *points, size_t num_points);
void dt_dev_coordinates_image_norm_to_preview_abs(dt_develop_t *dev, float *points, size_t num_points);
void dt_dev_coordinates_preview_abs_to_image_norm(dt_develop_t *dev, float *points, size_t num_points);
void dt_dev_coordinates_image_abs_to_raw_norm(dt_develop_t *dev, float *points, size_t num_points);

/**
 * @brief Get a point position from widget space to preview buffer space [0..1].
 * 
 * NOTE: The input point coordinates are without border subtraction.
 * 
 * @param dev the develop instance
 * @param px the x point coordinate in widget space, with no border subtraction.
 * @param py the y point coordinate in widget space, with no border subtraction.
 * @param mouse_x the returned x point coordinate relative to processed image [0..1].
 * @param mouse_y the returned y point coordinate relative to processed image [0..1].
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
 * modulegroups helpers
 */
/** request modulegroups to show the group of the given module */
void dt_dev_modulegroups_switch(dt_develop_t *dev, struct dt_iop_module_t *module);
/** update modulegroup visibility */
void dt_dev_modulegroups_update_visibility(dt_develop_t *dev);
/** reorder the module list */
void dt_dev_reorder_gui_module_list(dt_develop_t *dev);

/** request snapshot */
void dt_dev_snapshot_request(dt_develop_t *dev, const char *filename);

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
gchar *dt_dev_get_multi_name(const struct dt_iop_module_t *module);
gchar *dt_history_item_get_name(const struct dt_iop_module_t *module);
gchar *dt_history_item_get_name_html(const struct dt_iop_module_t *module);

/** generate item multi-instance name with mnemonics, for Gtk labels */
gchar *dt_history_item_get_label(const struct dt_iop_module_t *module);


/*
 * distort functions
 */
/** apply all transforms to the specified points (in virtual preview-pipe space) */
int dt_dev_coordinates_raw_abs_to_image_abs(dt_develop_t *dev, float *points, size_t points_count);
/** reverse apply all transforms to the specified points (in virtual preview-pipe space) */
int dt_dev_coordinates_image_abs_to_raw_abs(dt_develop_t *dev, float *points, size_t points_count);
/** same fct, but we can specify iop with priority between pmin and pmax */
int dt_dev_distort_transform_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                  float *points, size_t points_count);
/** same fct, but can only be called from a distort_transform function called by dt_dev_distort_transform_plus */
int dt_dev_distort_transform_locked(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order,
                                    const int transf_direction, float *points, size_t points_count);
/** same fct as dt_dev_coordinates_image_abs_to_raw_abs, but we can specify iop with priority between pmin and pmax */
int dt_dev_distort_backtransform_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                      float *points, size_t points_count);

/** get the iop_pixelpipe instance corresponding to the iop in the given pipe */
struct dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
                                                           struct dt_iop_module_t *module);

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

// This needs to run after `dt_dev_pixelpipe_get_roi_out()` so `pipe->processed_width`
// and `pipe->processed_height` are defined.
// Natural scale is the rescaling factor such that the full-res pipeline output
// (real or virtual) fits within darkroom widget area (minus borders/margins)
float dt_dev_get_natural_scale(dt_develop_t *dev);

// Get the final size of the main thumbnail that fits within darkroom central widget
// Needs to be recomputed when module parameters change (for modules changing ROI)
// or when the widget is resized.
int dt_dev_get_thumbnail_size(dt_develop_t *dev);

/**
 * @brief  Get the overlay scale factor
 * (scaling * natural_scale_on_processed_size * ppd)
 * 
 * @param dev the develop instance
 * @return float :the overlay scale factor
 */
float dt_dev_get_overlay_scale(dt_develop_t *dev);

/**
 * @brief Get the scale factor to fit the image into the darkroom area.
 * (scaling * natural_scale_on_processed_size)
 * 
 * @param dev the develop instance
 * @return float : the fit scale factor
 */
float dt_dev_get_fit_scale(dt_develop_t *dev);

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
 * @brief Convert absolute output-image coordinates to input image space by
 * calling `dt_dev_coordinates_image_abs_to_raw_abs()` directly, then normalize with
 * `dt_dev_coordinates_raw_abs_to_raw_norm()` when normalized raw coordinates
 * are required.
 */

// Update the mouse bounding box size according to current zoom level, dpp and DPI.
void dt_dev_update_mouse_effect_radius(dt_develop_t *dev);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
