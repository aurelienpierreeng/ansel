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

#include "common/atomic.h"
#include "common/image.h"
#include "common/imageio.h"
#include "common/iop_order.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_cache.h"


/**
 * struct used by iop modules to connect to pixelpipe.
 * data can be used to store whatever private data and
 * will be freed at the end.
 */
struct dt_iop_module_t;
struct dt_dev_raster_mask_t;
struct dt_iop_order_iccprofile_info_t;

typedef struct dt_dev_pixelpipe_raster_mask_t
{
  int id; // 0 is reserved for the reusable masks written in blend.c
  float *mask;
} dt_dev_pixelpipe_raster_mask_t;

typedef struct dt_dev_pixelpipe_iop_t
{
  struct dt_iop_module_t *module;  // the module in the dev operation stack
  struct dt_dev_pixelpipe_t *pipe; // the pipe this piece belongs to
  void *data;                      // to be used by the module to store stuff per pipe piece

  // Memory size of *data upon which we will compute integrity hashes.
  // This needs to be the size of the constant part of the data structure.
  // It can even be 0 if nothing relevant to cache integrity hashes is held there.
  // If the data struct contains pointers, they should go at the end of the struct,
  // and the size here should be adjusted to only include constant bits, starting at the address of *data.
  // "Constant" means identical between 2 pipeline nodes init,
  // because the lifecycle of a pixelpipe cache is longer than that of a pixelpipe itself.
  // See an example in colorbalancergb.c
  size_t data_size;

  void *blendop_data;              // to be used by the module to store blendop per pipe piece
  gboolean enabled; // used to disable parts of the pipe for export, independent on module itself.

  dt_dev_request_flags_t request_histogram;              // (bitwise) set if you want an histogram captured
  dt_dev_histogram_collection_params_t histogram_params; // set histogram generation params
  uint32_t *histogram; // pointer to histogram data; histogram_bins_count bins with 4 channels each
  dt_dev_histogram_stats_t histogram_stats; // stats of captured histogram
  uint32_t histogram_max[4];                // maximum levels in histogram, one per channel

  int iwidth, iheight; // width and height of input buffer

  // Hash representing the current state of the params, blend params and enabled state of this individual module
  uint64_t hash;
  uint64_t blendop_hash;

  // Cumulative hash representing the current module hash and all the upstream modules from the pipeline,
  // for the current ROI.
  uint64_t global_hash;

  // Same as global hash but for raster masks
  uint64_t global_mask_hash;

  int bpc;             // bits per channel, 32 means float
  int colors;          // how many colors per pixel
  dt_iop_roi_t buf_in,
      buf_out;                // theoretical full buffer regions of interest, as passed through modify_roi_out
  dt_iop_roi_t planned_roi_in, planned_roi_out; // sizes planned ahead for cache hash
  int process_cl_ready;       // set this to 0 in commit_params to temporarily disable the use of process_cl
  int process_tiling_ready;   // set this to 0 in commit_params to temporarily disable tiling

  // the following are used internally for caching:
  dt_iop_buffer_dsc_t dsc_in, dsc_out, dsc_mask;

  // bypass the cache for this module
  gboolean bypass_cache;

  // Set to TRUE for modules that should mandatorily cache their output to the RAM
  // even when running on OpenCL. This is counter-productive for lightweight pixel ops
  // like scalar operations, due to memory copy overhead. Not for diffuse & sharpen
  gboolean force_opencl_cache;

  GHashTable *raster_masks; // GList* of dt_dev_pixelpipe_raster_mask_t
} dt_dev_pixelpipe_iop_t;

typedef enum dt_dev_pixelpipe_change_t
{
  DT_DEV_PIPE_UNCHANGED = 0,        // no event
  DT_DEV_PIPE_TOP_CHANGED = 1 << 0, // only params of top element changed
  DT_DEV_PIPE_REMOVE = 1 << 1,      // possibly elements of the pipe have to be removed
  DT_DEV_PIPE_SYNCH
  = 1 << 2, // all nodes up to end need to be synched, but no removal of module pieces is necessary
  DT_DEV_PIPE_ZOOMED = 1 << 3 // zoom event, preview pipe does not need changes
} dt_dev_pixelpipe_change_t;

typedef enum dt_dev_pixelpipe_status_t
{
  DT_DEV_PIXELPIPE_DIRTY = 0,   // history stack changed or image new
  DT_DEV_PIXELPIPE_UNDEF = 1,   // pixelpipe computation started and we don't know yet
  DT_DEV_PIXELPIPE_VALID = 2,   // pixelpipe has finished; valid result
  DT_DEV_PIXELPIPE_INVALID = 3  // pixelpipe has finished; invalid result
} dt_dev_pixelpipe_status_t;

/**
 * this encapsulates the pixelpipe.
 * a develop module will need several of these:
 * for previews and full blits to cairo and for
 * the export function.
 */
typedef struct dt_backbuf_t
{
  size_t bpp;            // bits per pixel
  size_t width;          // pixel size of image
  size_t height;         // pixel size of image
  uint64_t hash;         // data checksum/integrity hash, for example to connect to a cacheline
  uint64_t history_hash; // arbitrary state hash
} dt_backbuf_t;

typedef struct dt_dev_pixelpipe_t
{
  // input image. Will be fetched directly from mipmap cache
  int32_t imgid;
  dt_mipmap_size_t size;

  // width and height of full-resolution input buffer
  int iwidth, iheight;

  // dimensions of processed buffer assuming we take full-resolution input
  int processed_width, processed_height;

  // this one actually contains the expected output format,
  // and should be modified by process*(), if necessary.
  dt_iop_buffer_dsc_t dsc;

  dt_dev_pixelpipe_status_t status;

  /** work profile info of the image */
  struct dt_iop_order_iccprofile_info_t *work_profile_info;
  /** input profile info **/
  struct dt_iop_order_iccprofile_info_t *input_profile_info;
  /** output profile info **/
  struct dt_iop_order_iccprofile_info_t *output_profile_info;

  // instances of pixelpipe, stored in GList of dt_dev_pixelpipe_iop_t
  GList *nodes;
  // event flag
  dt_dev_pixelpipe_change_t changed;

  // backbuffer (output)
  dt_backbuf_t backbuf;

  // Validity checksum of whole pipeline, 
  // taken as the global hash of the last pipe node (module),
  // after the last synchronization between dev history and pipe nodes completed.
  // This is computed in dt_dev_pixelpipe_get_global_hash
  // ahead of processing image.
  uint64_t hash;

  dt_pthread_mutex_t busy_mutex;

  // the data for the luminance mask are kept in a buffer written by demosaic or rawprepare
  // as we have to scale the mask later ke keep roi at that stage
  float *rawdetail_mask_data;
  struct dt_iop_roi_t rawdetail_mask_roi;
  int want_detail_mask;

  int output_imgid;
  // processing is true when actual pixel computations are ongoing
  int processing;
  // running is true when the pipe thread is running, computing or idle
  int running;
  // shutting down?
  dt_atomic_int shutdown;
  // opencl enabled for this pixelpipe?
  int opencl_enabled;
  // opencl error detected?
  int opencl_error;
  // running in a tiling context?
  int tiling;
  // should this pixelpipe display a mask in the end?
  int mask_display;
  // should this pixelpipe completely suppressed the blendif module?
  int bypass_blendif;
  // input data based on this timestamp:
  int input_timestamp;
  dt_dev_pixelpipe_type_t type;
  // the final output pixel format this pixelpipe will be converted to
  dt_imageio_levels_t levels;
  // opencl device that has been locked for this pipe.
  int devid;
  // image struct as it was when the pixelpipe was initialized. copied to avoid race conditions.
  dt_image_t image;
  // the user might choose to overwrite the output color space and rendering intent.
  dt_colorspaces_color_profile_type_t icc_type;
  gchar *icc_filename;
  dt_iop_color_intent_t icc_intent;
  // snapshot of modules iop_order
  GList *iop_order_list;
  // snapshot of mask list
  GList *forms;
  // the masks generated in the pipe for later reusal are inside dt_dev_pixelpipe_iop_t
  gboolean store_all_raster_masks;

  // hash of the last history item synchronized with pipeline
  // that's because the sync_top option can't assume only one history
  // item was added since the last synchronization.
  uint64_t last_history_hash;

  // hash of the whole history stack at the time of synchonization
  // between pipe and history. This is a local copy of 
  // dev_history_get_hash()
  uint64_t history_hash;

  // Modules can set this to TRUE internally so the pipeline will
  // restart right away, in the same thread.
  // The reentry flag can only be reset (to FALSE) by the same object that captured it.
  // DO NOT SET THAT DIRECTLY, use the setter/getter functions
  gboolean reentry;

  // Unique identifier of the object capturing the reentry flag.
  // This can be a mask or module hash, or anything that stays constant
  // across 2 pipeline runs from a same thread (aka as long as we don't reinit).
  // DO NOT SET THAT DIRECTLY, use the setter/getter functions
  uint64_t reentry_hash;

  // Can be set arbitrarily by pixelpipe modules at runtime
  // to invalidate downstream module cache lines.
  // This always gets reset to FALSE when a pipeline finishes,
  // whether on success or on error.
  gboolean flush_cache;

  // TRUE if at least one module is bypassing the cache
  gboolean bypass_cache;

  // Timeout in usec to delay the start of the pipeline
  // Use for example if you want to ensure one pipe starts after another.
  // Timeout is reset to 0 after the pipeline has run.
  int timeout;

} dt_dev_pixelpipe_t;

struct dt_develop_t;

#ifdef __cplusplus
extern "C" {
#endif

char *dt_pixelpipe_get_pipe_name(dt_dev_pixelpipe_type_t pipe_type);

// inits the pixelpipe with plain passthrough input/output and empty input and default caching settings.
int dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe);
// inits the preview pixelpipe with plain passthrough input/output and empty input and default caching
// settings.
int dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe);
// inits the pixelpipe with settings optimized for full-image export (no history stack cache)
int dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int levels, gboolean store_masks);
// inits the pixelpipe with settings optimized for thumbnail export (no history stack cache)
int dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe);
// inits all but the pixel caches, so you can't actually process an image (just get dimensions and
// distortions)
int dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe);
// inits the pixelpipe
int dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe);
// constructs a new input buffer from given RGB float array.
void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int32_t imgid, int width,
                                int height, dt_mipmap_size_t size);
// set some metadata for colorout to avoid race conditions.
void dt_dev_pixelpipe_set_icc(dt_dev_pixelpipe_t *pipe, dt_colorspaces_color_profile_type_t icc_type,
                              const gchar *icc_filename, dt_iop_color_intent_t icc_intent);
// destroys all allocated data.
void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe);
// cleanup all nodes except clean input/output
void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe);
// sync with develop_t history stack from scratch (new node added, have to pop old ones)
// this should be called with dev->history_mutex locked in read mode
void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// sync with develop_t history stack by just copying the top item params (same op, new params on top)
void dt_dev_pixelpipe_synch_all_real(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, const char *caller_func);
#define dt_dev_pixelpipe_synch_all(pipe, dev) dt_dev_pixelpipe_synch_all_real(pipe, dev, __FUNCTION__)
// adjust output node according to history stack (history pop event)
void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);

// process region of interest of pixels. returns 1 if pipe was altered during processing.
int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, dt_iop_roi_t roi);

// disable given op and all that comes after it in the pipe:
void dt_dev_pixelpipe_disable_after(dt_dev_pixelpipe_t *pipe, const char *op);
// disable given op and all that comes before it in the pipe:
void dt_dev_pixelpipe_disable_before(dt_dev_pixelpipe_t *pipe, const char *op);

// helper function to pass a raster mask through a (so far) processed pipe
// `*error` will be set to 1 if the raster mask reference couldn't be found while it should have been,
// aka not if user has forgotten to input what module should provide its mask, but only
// if the mask reference has been lost by the pipeline. This should lead to a pipeline cache flushing.
// `*error` can be NULL, e.g. for non-cached pipelines (export, thumbnail).
float *dt_dev_get_raster_mask(dt_dev_pixelpipe_t *pipe, const struct dt_iop_module_t *raster_mask_source,
                              const int raster_mask_id, const struct dt_iop_module_t *target_module,
                              gboolean *free_mask, int *error);

// some helper functions related to the details mask interface
void dt_dev_clear_rawdetail_mask(dt_dev_pixelpipe_t *pipe);

gboolean dt_dev_write_rawdetail_mask(dt_dev_pixelpipe_iop_t *piece, float *const rgb, const dt_iop_roi_t *const roi_in, const int mode);
#ifdef HAVE_OPENCL
gboolean dt_dev_write_rawdetail_mask_cl(dt_dev_pixelpipe_iop_t *piece, cl_mem in, const dt_iop_roi_t *const roi_in, const int mode);
#endif

// helper function writing the pipe-processed ctmask data to dest
float *dt_dev_distort_detail_mask(const dt_dev_pixelpipe_t *pipe, float *src, const struct dt_iop_module_t *target_module);


/**
 * @brief Set the re-entry pipeline flag, only if no object is already capturing it.
 * Re-entered pipelines run with cache disabled, but without flushing the whole cache.
 * This was designed for cases where raster masks references are lost on pipeline,
 * for example when going to lighttable and re-entering darkroom (pipe caches are not flushed
 * for performance, if re-entering the same image), as to trigger a full pipe run
 * and reinit references.
 *
 * It can be used for any case where a full pipeline recompute is needed once,
 * based on runtime module requirements, but a full cache flush would be overkill.
 *
 * NOTE: in main darkroom pipe, the coordinates of the ROI can change between
 * runs from the same thread.
 *
 * @param pipe
 * @param hash Unique ID of the object attempting capture the re-entry flag.
 * This should stay constant between 2 pipeline runs from the same thread.
 * @return gboolean TRUE if the object could capture the flag
 */
gboolean dt_dev_pixelpipe_set_reentry(dt_dev_pixelpipe_t *pipe, uint64_t hash);

/**
 * @brief Remove the re-entry pipeline flag, only if the object identifier is the one that set it.
 * See `dt_dev_pixelpipe_set_reentry`.
 *
 * @param pipe
 * @param hash Unique ID of the object attempting capture the re-entry flag.
 * This should stay constant between 2 pipeline runs from the same thread.
 * @return gboolean TRUE if the object could capture the flag
 */
gboolean dt_dev_pixelpipe_unset_reentry(dt_dev_pixelpipe_t *pipe, uint64_t hash);

// check if pipeline should re-entry after it completes
gboolean dt_dev_pixelpipe_has_reentry(dt_dev_pixelpipe_t *pipe);

// Force-reset pipeline re-entry flag, for example if we lost the unique ID of the object
// in a re-entry loop.
void dt_dev_pixelpipe_reset_reentry(dt_dev_pixelpipe_t *pipe);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
