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

#include <gmodule.h>
#include <gtk/gtk.h>
#include <sched.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** region of interest */
typedef struct dt_iop_roi_t
{
  int x, y, width, height;
  float scale;
} dt_iop_roi_t;

#ifdef __cplusplus
}
#endif

#include "common/darktable.h"
#include "common/introspection.h"
#include "common/gui_module_api.h"
#include "common/opencl.h"

#include "control/settings.h"
#include "develop/pixelpipe.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dt_develop_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;
struct dt_develop_blend_params_t;
struct dt_develop_tiling_t;
struct dt_iop_color_picker_t;

typedef enum dt_iop_module_header_icons_t
{
  IOP_MODULE_SWITCH = 0,
  IOP_MODULE_ICON,
  IOP_MODULE_LABEL,
  IOP_MODULE_MASK,
  IOP_MODULE_INSTANCE,
  IOP_MODULE_RESET,
  IOP_MODULE_PRESETS,
  IOP_MODULE_LAST
} dt_iop_module_header_icons_t;

/** module group */
typedef enum dt_iop_group_t
{
  IOP_GROUP_NONE = 0,
  IOP_GROUP_TONES = 1,
  IOP_GROUP_FILM = 2,
  IOP_GROUP_COLOR = 3,
  IOP_GROUP_REPAIR = 4,
  IOP_GROUP_SHARPNESS = 5,
  IOP_GROUP_EFFECTS = 6,
  IOP_GROUP_TECHNICAL= 7,
  IOP_GROUP_LAST
} dt_iop_group_t;

/** module tags */
typedef enum dt_iop_tags_t
{
  IOP_TAG_NONE = 0,
  IOP_TAG_DISTORT = 1 << 0,
  IOP_TAG_DECORATION = 1 << 1,
  IOP_TAG_CLIPPING = 1 << 2,

  // might be some other filters togglable by user?
  // IOP_TAG_SLOW       = 1<<3,
  // IOP_TAG_DETAIL_FIX = 1<<3,
} dt_iop_tags_t;

/** module tags */
typedef enum dt_iop_flags_t
{
  IOP_FLAGS_NONE = 0,

  /** Flag for the iop module to be enabled/included by default when creating a style */
  IOP_FLAGS_INCLUDE_IN_STYLES = 1 << 0,
  IOP_FLAGS_SUPPORTS_BLENDING = 1 << 1, // Does provide blending modes
  IOP_FLAGS_DEPRECATED = 1 << 2,
  IOP_FLAGS_ALLOW_TILING = 1 << 3, // Does allow tile-wise processing (valid for CPU and GPU processing)
  IOP_FLAGS_HIDDEN = 1 << 4,       // Hide the iop from userinterface
  IOP_FLAGS_TILING_FULL_ROI
  = 1 << 5, // Tiling code has to expect arbitrary roi's for this module (incl. flipping, mirroring etc.)
  IOP_FLAGS_ONE_INSTANCE = 1 << 6,       // The module doesn't support multiple instances
  IOP_FLAGS_PREVIEW_NON_OPENCL = 1 << 7, // Preview pixelpipe of this module must not run on GPU but always on CPU
  IOP_FLAGS_NO_HISTORY_STACK = 1 << 8,   // This iop will never show up in the history stack
  IOP_FLAGS_NO_MASKS = 1 << 9,          // The module doesn't support masks (used with SUPPORT_BLENDING)
  IOP_FLAGS_FENCE = 1 << 10,             // No module can be moved pass this one
  IOP_FLAGS_UNSAFE_COPY = 1 << 11,       // Unsafe to copy as part of history
  IOP_FLAGS_GUIDES_SPECIAL_DRAW = 1 << 12, // handle the grid drawing directly
  IOP_FLAGS_INTERNAL_MASKS = 1 << 13     // Module uses masks internally, outside of blendops. This advertises the need to commit them to history unconditionnaly.
} dt_iop_flags_t;

typedef struct dt_iop_gui_data_t
{
  // "base type" for all dt_iop_XXXX_gui_data_t types used by iops
  // to avoid compiler error about different sizes of empty structs between C and C++, we need at least one member
  int dummy;
} dt_iop_gui_data_t;

typedef void dt_iop_data_t;
typedef void dt_iop_global_data_t;

/** color picker request */
typedef enum dt_dev_request_colorpick_flags_t
{
  DT_REQUEST_COLORPICK_OFF = 0,   // off
  DT_REQUEST_COLORPICK_MODULE = 1 // requested by module (should take precedence)
} dt_dev_request_colorpick_flags_t;

/** colorspace enums, must be in synch with dt_iop_colorspace_type_t in color_conversion.cl */
typedef enum dt_iop_colorspace_type_t
{
  IOP_CS_NONE = -1,
  IOP_CS_RAW = 0,
  IOP_CS_LAB = 1,
  IOP_CS_RGB = 2,
  IOP_CS_LCH = 3,
  IOP_CS_HSL = 4,
  IOP_CS_JZCZHZ = 5,
} dt_iop_colorspace_type_t;

/** part of the module which only contains the cached dlopen stuff. */
typedef struct dt_iop_module_so_t
{
  // Needs to stay on top for casting
  dt_gui_module_t common_fields;

#define INCLUDE_API_FROM_MODULE_H
#include "iop/iop_api.h"

  /** opened module. */
  GModule *module;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** other stuff that may be needed by the module, not only in gui mode. inited only once, has to be
   * read-only then. */
  dt_iop_global_data_t *data;
  /** gui is also only inited once at startup. */
//  dt_iop_gui_data_t *gui_data;
  /** which results in this widget here, too. */

  void (*process_plain)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                      const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                      const struct dt_iop_roi_t *const roi_out);

  // introspection related data
  gboolean have_introspection;
} dt_iop_module_so_t;

typedef struct dt_iop_module_t
{
  // Needs to stay on top for casting
  dt_gui_module_t common_fields;

#define INCLUDE_API_FROM_MODULE_H
#include "iop/iop_api.h"

  /** opened module. */
  GModule *module;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** used to identify this module in the history stack. */
  int32_t instance;
  /** order of the module on the pipe. the pipe will be sorted by iop_order. */
  int iop_order;
  /** module sets this if the enable checkbox should be hidden. */
  int32_t hide_enable_button;
  /** set to DT_REQUEST_COLORPICK_MODULE if you want an input color picked during next eval. gui mode only. */
  dt_dev_request_colorpick_flags_t request_color_pick;
  /** (bitwise) set if you want an histogram generated during next eval */
  dt_dev_request_flags_t request_histogram;
  /** set to 1 if you want the mask to be transferred into alpha channel during next eval. gui mode only. */
  int request_mask_display;
  /** set to 1 if you want the blendif mask to be suppressed in the module in focus. gui mode only. */
  int32_t suppress_mask;
  /** set to 1 if the pipeline cache needs to be bypassed for downstream modules starting from this module*/
  gboolean bypass_cache;
  /** place to store the picked color of module input. */
  dt_aligned_pixel_t picked_color, picked_color_min, picked_color_max;
  /** place to store the picked color of module output (before blending). */
  dt_aligned_pixel_t picked_output_color, picked_output_color_min, picked_output_color_max;
  /** pointer to pre-module histogram data; if available: histogram_bins_count bins with 4 channels each */
  uint32_t *histogram;
  /** stats of captured histogram */
  dt_dev_histogram_stats_t histogram_stats;
  /** maximum levels in histogram, one per channel */
  uint32_t histogram_max[4];
  /** requested colorspace for the histogram, valid options are:
   * IOP_CS_NONE: module colorspace
   * IOP_CS_LCH: for Lab modules
   */
  dt_iop_colorspace_type_t histogram_cst;
  /** scale the histogram so the middle grey is at .5 */
  int histogram_middle_grey;
  /** the module is used in this develop module. */
  struct dt_develop_t *dev;
  /** non zero if this node should be processed. */
  gboolean enabled;
  /** Legacy default-enabled modules that left no history if user didn't changed params, prior to Darktable 3.0
   * These modules will be forced enabled even for existing histories, when initing new histories.
   * Disabling them (if allowed) will require another history step.
  */
  gboolean default_enabled;

  gboolean workflow_enabled;
  /** parameters for the operation. will be replaced by history revert. */
  dt_iop_params_t *params, *default_params;
  /** size of individual params struct. */
  int32_t params_size;
  /** parameters needed if a gui is attached. will be NULL if in export/batch mode. */
  dt_iop_gui_data_t *gui_data;
  dt_pthread_mutex_t gui_lock;
  /** other stuff that may be needed by the module, not only in gui mode. */
  dt_iop_global_data_t *global_data;
  /** blending params */
  struct dt_develop_blend_params_t *blend_params, *default_blendop_params;
  /** holder for blending ui control */
  gpointer blend_data;
  struct {
    struct {
      /** if this module generates a mask, is it used later on? needed to decide if the mask should be stored.
          maps dt_iop_module_t* -> id
      */
      GHashTable *users;
      /** the masks this module has to offer. maps id -> name.
       * So for there is only one mask per module and its id is always 0.
      */
      GHashTable *masks;
    } source;
    struct {
      /** the module that provides the raster mask (if any). keep in sync with blend_params! */
      struct dt_iop_module_t *source;
      int id;
    } sink;
  } raster_mask;
  /** child widget which is added to the GtkExpander. copied from module_so_t. */
  GtkWidget *widget;
  /** off button, somewhere in header, common to all plug-ins. */
  GtkDarktableToggleButton *off;
  /** this is the module header, contains label and buttons */
  GtkWidget *header;
  /** this is the module mask indicator, inside header */
  GtkWidget *mask_indicator;
  /** expander containing the widget and flag to store expanded state */
  GtkWidget *expander;
  gboolean expanded;
  /** reset parameters button */
  GtkWidget *reset_button;
  /** show preset menu button */
  GtkWidget *presets_button;
  /** fusion slider */
  GtkWidget *fusion_slider;

  /** show/hide guide button and combobox */
  GtkWidget *guides_toggle;
  GtkWidget *guides_combo;

  /** the corresponding SO object */
  dt_iop_module_so_t *so;

  /** multi-instances things */
  int multi_priority; // user may change this
  char multi_name[128]; // user may change this name
  gboolean multi_show_close;
  gboolean multi_show_up;
  gboolean multi_show_down;
  gboolean multi_show_new;
  GtkWidget *multimenu_button;

  /** delayed-event handling */
  guint timeout_handle;

  void (*process_plain)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                        const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                        const struct dt_iop_roi_t *const roi_out);

  // introspection related data
  gboolean have_introspection;

  // parameters hash
  uint64_t hash;

  // blendop hash
  // Ideally, this would be added to struct dt_develop_blend_params_t *blendop
  // but since blendops are dumped to DB as a memory blob, we can't change their length
  // without updating version (aka breaking backwards compatibility), and this minor
  // stuff is not worth it.
  uint64_t blendop_hash;

} dt_iop_module_t;

/** loads and inits the modules in the plugins/ directory. */
void dt_iop_load_modules_so(void);
/** cleans up the dlopen refs. */
void dt_iop_unload_modules_so(void);
/** load a module for a given .so */
int dt_iop_load_module_by_so(dt_iop_module_t *module, dt_iop_module_so_t *so, struct dt_develop_t *dev);
/** returns a list of instances referencing stuff loaded in load_modules_so. */
GList *dt_iop_load_modules_ext(struct dt_develop_t *dev, gboolean no_image);
GList *dt_iop_load_modules(struct dt_develop_t *dev);
int dt_iop_load_module(dt_iop_module_t *module, dt_iop_module_so_t *module_so, struct dt_develop_t *dev);
/** calls module->cleanup and closes the dl connection. */
void dt_iop_cleanup_module(dt_iop_module_t *module);
/** initialize pipe. */
void dt_iop_init_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                      struct dt_dev_pixelpipe_iop_t *piece);
/** checks if iop do have an ui */
gboolean dt_iop_so_is_hidden(dt_iop_module_so_t *module);
gboolean dt_iop_is_hidden(dt_iop_module_t *module);
/** enter a GUI critical section by acquiring gui_data->lock **/
static inline void dt_iop_gui_enter_critical_section(dt_iop_module_t *const module)
  ACQUIRE(&module->gui_lock)
{
  dt_pthread_mutex_lock(&module->gui_lock);
}
/** leave a GUI critical section by releasing gui_data->lock **/
static inline void dt_iop_gui_leave_critical_section(dt_iop_module_t *const module)
  RELEASE(&module->gui_lock)
{
  dt_pthread_mutex_unlock(&module->gui_lock);
}
/** cleans up gui of module and of blendops */
void dt_iop_gui_cleanup_module(dt_iop_module_t *module);
/** updates the enable button state. (take into account module->enabled and module->hide_enable_button  */
void dt_iop_gui_set_enable_button(dt_iop_module_t *module);
/** updates the gui params and the enabled switch. */
void dt_iop_gui_update(dt_iop_module_t *module);
/** reset the ui to its defaults */
void dt_iop_gui_reset(dt_iop_module_t *module);
/** set expanded state of iop */
void dt_iop_gui_set_expanded(dt_iop_module_t *module, gboolean expanded, gboolean collapse_others);
/** refresh iop according to set expanded state */
void dt_iop_gui_update_expanded(dt_iop_module_t *module);
/* duplicate module and return new instance */
dt_iop_module_t *dt_iop_gui_duplicate(dt_iop_module_t *base, gboolean copy_params);

void dt_iop_gui_update_header(dt_iop_module_t *module);

/** commits params and updates piece hash. */
void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params,
                          struct dt_develop_blend_params_t *blendop_params, struct dt_dev_pixelpipe_t *pipe,
                          struct dt_dev_pixelpipe_iop_t *piece);
void dt_iop_commit_blend_params(dt_iop_module_t *module, const struct dt_develop_blend_params_t *blendop_params);
/** make sure the raster mask is advertised if available */
void dt_iop_set_mask_mode(dt_iop_module_t *module, int mask_mode);
/** creates a label widget for the expander, with callback to enable/disable this module. */
void dt_iop_gui_set_expander(dt_iop_module_t *module);
/** get the widget of plugin ui in expander */
GtkWidget *dt_iop_gui_get_widget(dt_iop_module_t *module);
/** get the eventbox of plugin ui in expander */
GtkWidget *dt_iop_gui_get_pluginui(dt_iop_module_t *module);

/** requests the focus for this plugin (to draw overlays over the center image) */
void dt_iop_request_focus(dt_iop_module_t *module);
/** allocate and load default settings from introspection. */
void dt_iop_default_init(dt_iop_module_t *module);
/** loads default settings from database. */
void dt_iop_load_default_params(dt_iop_module_t *module);
/** creates the module's gui widget */
void dt_iop_gui_init(dt_iop_module_t *module);
/** reloads certain gui/param defaults when the image was switched. */
void dt_iop_reload_defaults(dt_iop_module_t *module);

/** allow plugins to relinquish CPU and go to sleep for some time */
void dt_iop_nap(int32_t usec);

/** get module by name and colorout, works only with a dev mode */
dt_iop_module_t *dt_iop_get_colorout_module(void);
/* returns the iop-module found in list with the given name */
dt_iop_module_t *dt_iop_get_module_from_list(GList *iop_list, const char *op);
dt_iop_module_t *dt_iop_get_module(const char *op);
/** returns module with op + multi_priority or NULL if not found on the list,
    if multi_priority == -1 do not check for it */
dt_iop_module_t *dt_iop_get_module_by_op_priority(GList *modules, const char *operation, const int multi_priority);
/** returns module with op + multi_name or NULL if not found on the list,
    if multi_name == NULL do not check for it */
dt_iop_module_t *dt_iop_get_module_by_instance_name(GList *modules, const char *operation, const char *multi_name);

/** return preferred module instance for shortcuts **/
dt_iop_module_t *dt_iop_get_module_preferred_instance(dt_iop_module_so_t *module);

/** returns true if module is the first instance of this operation in the pipe */
gboolean dt_iop_is_first_instance(GList *modules, dt_iop_module_t *module);


/** get module flags, works in dev and lt mode */
int dt_iop_get_module_flags(const char *op);

/** returns the localized plugin name for a given op name. must not be freed. */
const gchar *dt_iop_get_localized_name(const gchar *op);
const gchar *dt_iop_get_localized_aliases(const gchar *op);

/** set multi_priority and update raster mask links */
void dt_iop_update_multi_priority(dt_iop_module_t *module, int new_priority);

/** iterates over the users hash table and checks if a specific mask is being used */
gboolean dt_iop_is_raster_mask_used(dt_iop_module_t *module, int id);

/** returns the previous visible module on the module list */
dt_iop_module_t *dt_iop_gui_get_previous_visible_module(dt_iop_module_t *module);
/** returns the next visible module on the module list */
dt_iop_module_t *dt_iop_gui_get_next_visible_module(dt_iop_module_t *module);
/** check if current module is visible **/
gboolean dt_iop_gui_module_is_visible(dt_iop_module_t *module);

// initializes memory.darktable_iop_names
void dt_iop_set_darktable_iop_table();

/** queue a refresh of the center (FULL), preview, or second-preview windows, rerunning the pixelpipe from */
/** the given module */
void dt_iop_refresh_center(dt_iop_module_t *module);
void dt_iop_refresh_preview(dt_iop_module_t *module);

/** queue a delayed call to dt_dev_add_history_item to capture module parameters */
void dt_iop_queue_history_update(dt_iop_module_t *module, gboolean extend_prior);
/** cancel any previously-queued history update */
void dt_iop_cancel_history_update(dt_iop_module_t *module);

/** add/remove mask indicator to iop module header */
void dt_iop_add_remove_mask_indicator(dt_iop_module_t *module);

// format modules description going in tooltips
const char **dt_iop_set_description(dt_iop_module_t *module, const char *main_text,
                                    const char *purpose, const char *input,
                                    const char *process, const char *output);

static inline dt_iop_gui_data_t *_iop_gui_alloc(dt_iop_module_t *module, size_t size)
{
  // Align so that DT_ALIGNED_ARRAY may be used within gui_data struct
  module->gui_data = (dt_iop_gui_data_t*)dt_calloc_align(size);
  dt_pthread_mutex_init(&module->gui_lock,NULL);
  return module->gui_data;
}
#define IOP_GUI_ALLOC(module) \
  (dt_iop_##module##_gui_data_t *)_iop_gui_alloc(self,sizeof(dt_iop_##module##_gui_data_t))

#define IOP_GUI_FREE \
  dt_pthread_mutex_destroy(&self->gui_lock);if(self->gui_data){dt_free_align(self->gui_data);} self->gui_data = NULL

/** check whether we have the required number of channels in the input data; if not, copy the input buffer to the
 ** output buffer, set the module's trouble message, and return FALSE */
gboolean dt_iop_have_required_input_format(const int required_ch, struct dt_iop_module_t *const module,
                                           const int actual_pipe_ch,
                                           const void *const __restrict__ ivoid, void *const __restrict__ ovoid,
                                           const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out);

/* bring up module rename dialog */
void dt_iop_gui_rename_module(dt_iop_module_t *module);

/* dispatch post-value-changed GUI events within module and commit changes to history */
void dt_iop_gui_changed(dt_iop_module_t *module, GtkWidget *widget, gpointer data);

// update all bauhaus widgets in an iop module from their params fields
void dt_bauhaus_update_module(dt_iop_module_t *self);

// default callback to connect to value-changed signal for bauhaus widgets
// it will automatically call dt_iop_changed for the right module.
void dt_bauhaus_value_changed_default_callback(GtkWidget *widget);


/** Uniform way of getting the full state hash of user-defined parameters, including masks and blending.
 * Writes the value in module->hash, also writes the module->blendop_hash for masking and blending.
 *
 * WARNING: doesn't take into account parameters dynamically set at runtime.
 *
 * WARNING: if computing module hash in HISTORY order, there is no guaranty that
 * previous modules in PIPELINE are also previous modules in HISTORY,
 * so their blendops & masks may not be inited yet, meaning the blendop_hash we compute here
 * will be garbage. In particular, (legacy) history compression algo can mess with history order,
 * such that history entries of raster mask providers can end up after history entries of raster mask consumers.
 * Our current history compression does a pipeline snapshot in pipeline order, for user-changed modules.
 * (Raster masks providers and consumers will all be user-changed modules).
 *
 * IF COMPUTING HASHES FOR PIPE CACHE INVALIDATION, that means we need to redo blendop/module hash recomputation
 * at commit_params time, when pushing history to pipeline (but mandatorily in pipeline order).
 *
 * IF COMPUTING HASHES FOR HISTORY CONSISTENCY (auto-saving when needed), since all we care is user params,
 * it doesn't matter.
 *
*/
void dt_iop_compute_module_hash(dt_iop_module_t *module, GList *masks);

// Use module fingerprints to determine if two instances are actually the same
gboolean dt_iop_check_modules_equal(dt_iop_module_t *mod_1, dt_iop_module_t *mod_2);


/** Set bypass to TRUE if the pipeline cache should be bypassed temporarily for
 * this module and the next, for example doing interactive GUI operations.
 *
 * Pipeline cache consistency is ensured by hashing the internal module params
 * and comparing that hash with the previously-known one from the cache line.
 * If something outside the module is making the cache temporarily invalid,
 * this is the way to go. The previous module's output may be fetched from cache
 * if available, which is faster than simply disabling cache at all.
 *
 * This is designed for mask previews, which have a special handling, in pipeline,
 * where modules between the mask preview requesting module and the gamma.c
 * module are bypassed, so the alpha channel is passed-through to be rendered by
 * gamma.c without processing intermediate modules. This doesn't work if cache is
 * enabled.
 *
 * The pixelpipe code will propagate the bypass state to downstream pipe->pieces,
 * so all modules coming later than this one in the pipeline will also have their
 * cache disabled. That's how mask preview can work, because the pipeline is called
 * in a recursive way, starting from the end.
 *
 * Only one module from dev->iop list (modules tied to GUI) is allowed to bypass
 * the cache at a time, other modules will lose their bypass flag if set.
*/
gboolean dt_iop_get_cache_bypass(dt_iop_module_t *module);
void dt_iop_set_cache_bypass(dt_iop_module_t *module, gboolean state);

// after writing data using copy_pixel_nontemporal, it is necessary to
// ensure that the writes have completed before attempting reads from
// a different core.  This function produces the required memory
// fence to ensure proper visibility
static inline void dt_sfence()
{
#if defined(__SSE__)
  _mm_sfence();
#else
  // the following generates an MFENCE instruction on x86/x64.  We
  // only really need SFENCE, which is less expensive, but none of the
  // other memory orders generate *any* fence instructions on x64.
#ifdef __cplusplus
  std::atomic_thread_fence(std::memory_order_seq_cst);
#else
  atomic_thread_fence(memory_order_seq_cst);
#endif
#endif
}

// if the copy_pixel_nontemporal() writes were inside an OpenMP
// parallel loop, the OpenMP parallelization will have performed a
// memory fence before resuming single-threaded operation, so a
// dt_sfence would be superfluous.  But if compiled without OpenMP
// parallelization, we should play it safe and emit a memory fence.
// This function should be used right after a parallelized for loop,
// where it will produce a barrier only if needed.
#ifdef _OPENMP
#define dt_omploop_sfence()
#else
#define dt_omploop_sfence() dt_sfence()
#endif

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
