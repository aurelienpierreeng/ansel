/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Guillaume STUTIN.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel. If not, see <http://www.gnu.org/licenses/>.
*/

/** Studio Capture view: monitor a folder, import incoming shots, auto-apply
    styles and preview the active image full-size with the filmstrip at the
    bottom. The center view renders the processed image through the async
    surface fetcher (fit-to-window or 100% with panning); the darkroom editing
    panels are intentionally absent: this is a viewer, not an editor. */

#include "develop/masks_gui.h"
#include "system/atomic.h"
#include "widgets/widget_settings.h"
#include "common/collection.h"
#include "common/module_versioning.h"
#include "common/selection.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/dev_history.h"
#include "develop/dev_pixelpipe.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "caches/pixelpipe_cache.h"
#include "develop/pixelpipe_hb.h"
#include "gui/dtgtk/thumbtable.h"
#include "gui/color_picker_proxy.h"
#include "widgets/gdkkeys.h"
#include "gui/application.h"
#include "gui/guides.h"
#include "libs/colorpicker.h"
#include "libs/lib.h"
#include "views/dev_backbuf.h"
#include "views/dev_toolbox.h"
#include "views/view.h"
#include "views/view_api.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>
#include "control/signal.h"

DT_MODULE(1)

typedef struct dt_studio_capture_t
{
  dt_view_image_surface_fetcher_t fetcher;
  cairo_surface_t *surface;
  int32_t imgid;

  // Last known center size, updated on expose.
  int width;
  int height;

  // DT_THUMBTABLE_ZOOM_FIT or DT_THUMBTABLE_ZOOM_FULL (100% + panning)
  dt_thumbtable_zoom_t zoom;

  // Top-left corner of the viewport inside the 100% surface, in surface pixels.
  double pan_x;
  double pan_y;

  // The 100% surface arrives asynchronously: remember the relative image point
  // (0..1) that was under the pointer when zooming so the first 100% expose can
  // center the pan there.
  gboolean anchor_pending;
  double anchor_rel_x;
  double anchor_rel_y;
  double anchor_px;
  double anchor_py;

  // Pan dragging state
  gboolean panning;
  double drag_start_x;
  double drag_start_y;
  double pan_start_x;
  double pan_start_y;

  // Color picker box-drag state (image-normalized anchor corner)
  gboolean picker_dragging_box;
  float picker_box_anchor[2];

  // Deferred refresh after an import: styles are applied right after the
  // import signal fires, so the first fetched surface may predate them.
  guint refresh_timeout;

  // Own develop instance. While the atelier is active it is published as
  // darktable.develop and runs the pixelpipe on the displayed image so the
  // scopes (which read darktable.develop->preview_pipe) have data to show.
  // At DT_THUMBTABLE_ZOOM_FIT, expose() also prefers this pipe's own live
  // backbuf (main_locked/main_wait below) over the plain surface fetcher when
  // it is ready, since that is the only path that bakes in ISO 12646/
  // overexposed/raw overexposed/softproof/gamut. The fetcher's surface stays
  // the fallback (cold start, or 100% zoom, which this dev's ROI never
  // reflects - see _studio_configure_dev_roi) and the only source the color
  // picker's coordinate mapping (_studio_widget_to_image_norm) ever reads.
  dt_develop_t *dev;
  dt_develop_t *saved_develop; // global develop pointer to restore on leave
  gboolean dev_loaded;         // an image is loaded and its pipelines are set up
  int dev_width;               // last center allocation, for dt_dev_configure()
  int dev_height;

  // dev->pipe's live backbuf, GUI-side view (see dev_backbuf.h). Must be
  // released (dt_dev_release_locked_surface) before dev's pipeline nodes are
  // torn down - see _studio_dev_teardown().
  dt_dev_locked_surface_t main_locked;
  dt_dev_pixelpipe_cache_wait_t main_wait;

  // The Scopes module's "current pick" readout (numeric label + swatch) is a
  // GTK widget pair created once, at startup, hard-wired via signal user_data
  // to whichever develop's primary_sample was live at that moment: always
  // darkroom's. Our own dt_dev_init()-allocated primary_sample has no such
  // widgets, so its values compute correctly but never reach the screen. We
  // share darkroom's instance while active (own_primary_sample stashes ours)
  // and always restore it before dt_dev_cleanup() would otherwise free
  // darkroom's shared instance instead of our own.
  dt_colorpicker_sample_t *own_primary_sample;
} dt_studio_capture_t;

const char *name(const dt_view_t *self)
{
  return _("Studio Capture");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_STUDIO_CAPTURE;
}

void init(dt_view_t *self)
{
  dt_studio_capture_t *d = calloc(1, sizeof(dt_studio_capture_t));
  d->imgid = UNKNOWN_IMAGE;
  d->zoom = DT_THUMBTABLE_ZOOM_FIT;
  // calloc() zero-initializes main_locked.hash to 0, which looks like a valid hash, not the
  // "never locked" sentinel dt_dev_lock_pipe_surface()/dt_dev_release_locked_surface() expect.
  d->main_locked.hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  dt_view_image_surface_fetcher_init(&d->fetcher);

  // gui_attached = 1: the scopes only read a develop whose preview pipe is a
  // GUI-observable source (see the dev->gui_attached guards in libs/histogram.c).
  d->dev = (dt_develop_t *)malloc(sizeof(dt_develop_t));
  dt_dev_init(d->dev, 1);
  d->own_primary_sample = d->dev->color_picker.primary_sample;

  self->data = d;
}

void gui_init(dt_view_t *self)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  // Own instances, wired to this view's own dev, so the toggles' state and
  // their popovers' controls are actually correct here rather than mutating
  // darkroom's separate dev. The effect still won't be visible: it's
  // darkroom's own expose() that paints it on the pixelpipe backbuffer, a
  // path this view's surface-fetcher-based center never touches. Display's
  // popover only gets the generic controls (brightness, margins) here:
  // darkroom's own rendering-size and mask-preview-checkerboard extras don't
  // apply without an editing layer, so we don't append them.
  static const dt_dev_toolbox_button_t studio_capture_toolbox_buttons[]
      = { DT_DEV_TOOLBOX_ISO_12646,     DT_DEV_TOOLBOX_DISPLAY, DT_DEV_TOOLBOX_RAWOVEREXPOSED,
          DT_DEV_TOOLBOX_OVEREXPOSED, DT_DEV_TOOLBOX_SOFTPROOF, DT_DEV_TOOLBOX_GAMUT };
  dt_dev_toolbox_create(d->dev, DT_VIEW_STUDIO_CAPTURE, studio_capture_toolbox_buttons,
                       G_N_ELEMENTS(studio_capture_toolbox_buttons));
  gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(d->dev->display.floating_window)));

  // Accelerators for the same buttons, bound to the accel group this view
  // actually connects (see enter(): dt_accels_connect_active_group(...,
  // "lighttable")) instead of darkroom's darkroom_accels.
  dt_dev_toolbox_add_accels(d->dev, dt_gui_get_accels()->lighttable_accels, N_("Studio capture/Toolbox"),
                            studio_capture_toolbox_buttons, G_N_ELEMENTS(studio_capture_toolbox_buttons));
}

void cleanup(dt_view_t *self)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;
  if(d->refresh_timeout) g_source_remove(d->refresh_timeout);
  dt_view_image_surface_fetcher_invalidate(&d->fetcher, &d->surface);
  dt_view_image_surface_fetcher_cleanup(&d->fetcher);
  // Guard against leave() never having run (e.g. app shutdown while this view
  // was active): dt_dev_cleanup() must free our own primary sample, never
  // darkroom's shared one that enter() may have swapped in.
  d->dev->color_picker.primary_sample = d->own_primary_sample;
  dt_dev_cleanup(d->dev);
  dt_free(d->dev);
  dt_free(self->data);
}

int try_enter(dt_view_t *self)
{
  // The viewer can always be entered; it shows whatever image the filmstrip
  // points at, or an empty center until an image is selected or imported.
  return 0;
}

/**
 * @brief Load an image into the viewer develop and start its pipelines.
 *
 * This mirrors the pipeline-relevant subset of darkroom's enter: it loads the
 * processing modules and history, then starts the pipelines. It deliberately
 * omits the darkroom editing layer (IOP module GUIs, color picker, undo,
 * accels), which this viewer does not use.
 *
 * Requires darktable.develop == d->dev (set by enter()).
 */
/**
 * @brief Feed the current center allocation into the dev's ROI and re-derive
 * border_size from dev->iso_12646.enabled (dt_dev_toolbox_apply_iso_12646_size()
 * calls dt_dev_configure() internally). Centralizes what _studio_dev_setup(),
 * configure() and expose()'s resize catch-up all need, so toggling ISO 12646
 * survives a resize instead of being stomped back to border_size=0.
 */
static void _studio_configure_dev_roi(dt_studio_capture_t *d, int width, int height)
{
  dt_dev_viewport_set_widget_size(d->dev, width, height);
  dt_dev_toolbox_apply_iso_12646_size(d->dev);
}

static void _studio_dev_setup(dt_studio_capture_t *d, const int32_t imgid)
{
  dt_develop_t *dev = d->dev;

  dev->exit = 0;
  dt_masks_gui_init(dev);
  dev->gui_module = NULL;

  if(IS_NULL_PTR(dev->iop)) dev->iop = dt_dev_load_modules(dev);

  // From here the develop holds per-image resources that teardown must release.
  d->dev_loaded = TRUE;

  if(dt_dev_load_image(dev, imgid))
  {
    dt_control_log(_("Studio capture: could not load the image in the viewer."));
    return;
  }

  dt_dev_pop_history_items(dev);
  dt_dev_pixelpipe_rebuild_all(dev);
  dt_dev_get_thumbnail_size(dev);

  // Size the pipelines to the current center allocation so the preview pipe
  // (the scopes' source) produces a valid buffer.
  if(d->dev_width > 0 && d->dev_height > 0)
    _studio_configure_dev_roi(d, d->dev_width, d->dev_height);

  dt_dev_start_all_pipelines(dev);
}

/**
 * @brief Tear down the viewer develop pipelines and per-image resources.
 *
 * Faithful subset of darkroom's leave: shut the pipes down, wait on each busy
 * lock, drop the nodes and cached buffers, then free history, modules and
 * masks. Safe to call when only the modules were loaded (image load failed):
 * the pipeline teardown is harmless on pipes that never started.
 *
 * dt_iop_gui_cleanup_module() runs for every module before dt_free(), even
 * though this viewer itself never calls dt_iop_gui_init(): darktable.develop
 * is swapped to point at this view's dev while active (see enter()), and
 * global menu actions that read darktable.develop directly instead of a
 * locally-scoped dev (dt_apply_dev_history_update() callers in
 * gui/actions/edit.c, styles.c) can reach dt_dev_history_gui_update() on
 * THIS dev -- gui_attached is TRUE here too, so nothing stops it from
 * running dt_iop_gui_init()/dt_iop_gui_set_expander() on these modules.
 * Skipping gui_cleanup_module() then left header/expander widgets (each
 * g_object_weak_ref()-ed back to the module in dt_iop_gui_set_expander())
 * outliving the dt_free(module) below, use-after-freed the next time
 * something destroyed those widgets (ASan heap-use-after-free in
 * _iop_gui_widget_gone(), imageop.c:2020, freed by this loop, allocated by
 * dt_dev_load_modules() in _studio_dev_setup()).
 */
static void _studio_dev_teardown(dt_studio_capture_t *d)
{
  if(!d->dev_loaded) return;
  d->dev_loaded = FALSE;

  // Must happen before the pipe nodes/backbufs below are torn down: main_locked otherwise keeps
  // referencing a cache entry that is about to be invalidated/reused by the next image's pipe.
  dt_dev_release_locked_surface(&d->main_locked);
  dt_dev_pixelpipe_cache_wait_cleanup(&d->main_wait, "studio-capture-release-main");

  dt_develop_t *dev = d->dev;

  dev->exit = 1;
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);
  if(dev->virtual_pipe) dt_atomic_set_int(&dev->virtual_pipe->shutdown, TRUE);
  dev->pipelines_started = FALSE;

  // Stop module background threads before freeing the nodes/history they read.
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m->data;
    if(mod && mod->quiesce) mod->quiesce(mod);
  }

  // Taking each busy lock waits for the running pipe to release it.
  dt_pthread_mutex_lock(&dev->pipe->busy_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
  dt_dev_pixelpipe_cache_unref_hash(dt_dev_backbuf_get_hash(&dev->pipe->backbuf));
  dt_dev_set_backbuf(&dev->pipe->backbuf, 0, 0, 0, DT_PIXELPIPE_CACHE_HASH_INVALID, DT_PIXELPIPE_CACHE_HASH_INVALID);
  dt_pthread_mutex_unlock(&dev->pipe->busy_mutex);

  dt_pthread_mutex_lock(&dev->preview_pipe->busy_mutex);
  dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
  dt_dev_pixelpipe_cache_unref_hash(dt_dev_backbuf_get_hash(&dev->preview_pipe->backbuf));
  dt_dev_set_backbuf(&dev->preview_pipe->backbuf, 0, 0, 0, DT_PIXELPIPE_CACHE_HASH_INVALID,
                     DT_PIXELPIPE_CACHE_HASH_INVALID);
  dt_pthread_mutex_unlock(&dev->preview_pipe->busy_mutex);

  if(dev->virtual_pipe)
  {
    dt_pthread_mutex_lock(&dev->virtual_pipe->busy_mutex);
    dt_dev_pixelpipe_cleanup_nodes(dev->virtual_pipe);
    dt_dev_pixelpipe_cache_unref_hash(dt_dev_backbuf_get_hash(&dev->virtual_pipe->backbuf));
    dt_dev_set_backbuf(&dev->virtual_pipe->backbuf, 0, 0, 0, DT_PIXELPIPE_CACHE_HASH_INVALID,
                       DT_PIXELPIPE_CACHE_HASH_INVALID);
    dt_pthread_mutex_unlock(&dev->virtual_pipe->busy_mutex);
  }

  dt_dev_pixelpipe_cache_flush_clmem_for_pipe(dev->pipe->last_devid);
  if(dev->preview_pipe->last_devid != dev->pipe->last_devid)
    dt_dev_pixelpipe_cache_flush_clmem_for_pipe(dev->preview_pipe->last_devid);

  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_history_free_history(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  while(dev->iop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(dev->iop->data);
    if(!dt_iop_is_hidden(module)) dt_iop_gui_cleanup_module(module);
    dt_iop_cleanup_module(module);
    dt_free(module);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  while(dev->alliop)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)dev->alliop->data;
    if(!dt_iop_is_hidden(module)) dt_iop_gui_cleanup_module(module);
    dt_iop_cleanup_module(module);
    dt_free(module);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  dev->iop = dev->alliop = NULL;

  if(dev->form_gui)
  {
    dev->gui_module = NULL;
    dt_masks_gui_cleanup(dev);
    // dt_masks_gui_cleanup() frees but does not clear the pointer; NULL it so a
    // teardown not followed by a setup (reset to no image) cannot double-free.
    dev->form_gui = NULL;
  }

  dt_pthread_rwlock_wrlock(&dev->masks_mutex);
  // dev->forms and dev->allforms are independent claims on possibly-shared objects:
  // release both, do not unconditionally free -- a form referenced by both only
  // reaches refcount 0 once (mirrors dt_dev_cleanup() in develop.c).
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_form_unref);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_form_unref);
  dev->allforms = NULL;
  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  dev->image_storage.id = -1;

  // Release the histogram backbuf cache references.
  dt_dev_pixelpipe_cache_unref_hash(dt_dev_backbuf_get_hash(&dev->raw_histogram));
  dt_dev_backbuf_set_hash(&dev->raw_histogram, -1);
  dt_dev_pixelpipe_cache_unref_hash(dt_dev_backbuf_get_hash(&dev->output_histogram));
  dt_dev_backbuf_set_hash(&dev->output_histogram, -1);
  dt_dev_pixelpipe_cache_unref_hash(dt_dev_backbuf_get_hash(&dev->display_histogram));
  dt_dev_backbuf_set_hash(&dev->display_histogram, -1);
}

/**
 * @brief Switch the displayed image and reset the viewport to fit.
 */
static void _studio_set_image(dt_studio_capture_t *d, const int32_t imgid)
{
  if(imgid == d->imgid) return;

  // Drop the previous image's pipeline before loading the next.
  _studio_dev_teardown(d);

  d->imgid = imgid;
  d->zoom = DT_THUMBTABLE_ZOOM_FIT;
  d->pan_x = d->pan_y = 0.0;
  d->anchor_pending = FALSE;
  d->panning = FALSE;
  dt_view_image_surface_fetcher_invalidate(&d->fetcher, &d->surface);

  dt_view_active_images_reset(FALSE);
  if(imgid > UNKNOWN_IMAGE)
  {
    dt_view_active_images_add(imgid, TRUE);
    // Keep the library selection in sync with whatever this view displays
    // (a new import, a filmstrip activate...): darkroom's try_enter() falls
    // back to the selection, not to our own active-image tracking, and the
    // user expects "what's shown" and "what's selected" to be the same thing.
    dt_selection_select_single(dt_selection_get_global(), imgid);
    // darkroom's try_enter() checks mouse_over_id BEFORE falling back to the
    // selection, and its own enter() sets both mouse_over_id and
    // keyboard_over_id to the image it is about to load. Mirror that here so
    // switching to darkroom from Studio Capture resolves to the displayed
    // image even if a stale mouse_over_id is lingering from filmstrip hover.
    dt_control_set_mouse_over_id(imgid);
    dt_control_set_keyboard_over_id(imgid);
    // Only feed the scopes while the atelier owns the global develop pointer.
    if(dt_dev_get_global() == d->dev) _studio_dev_setup(d, imgid);
  }

  dt_control_queue_redraw_center();
}

static void _studio_filmstrip_activate_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  if(imgid > UNKNOWN_IMAGE) _studio_set_image((dt_studio_capture_t *)self->data, imgid);
}

/**
 * @brief Re-fetch the displayed surface once, after auto-styles had time to land.
 */
static gboolean _studio_deferred_refresh(gpointer user_data)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)user_data;
  d->refresh_timeout = 0;
  dt_view_image_surface_fetcher_invalidate(&d->fetcher, &d->surface);
  dt_control_queue_redraw_center();
  return G_SOURCE_REMOVE;
}

/**
 * @brief Follow the shooting session: every new import becomes the displayed image.
 */
static void _studio_image_imported_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;
  if(imgid <= UNKNOWN_IMAGE) return;

  _studio_set_image(d, imgid);

  // The import signal fires before the survey styles are applied to the new
  // image, so the surface fetched right away may show the unstyled state.
  // Schedule one deferred re-fetch after the styles have been written.
  if(d->refresh_timeout) g_source_remove(d->refresh_timeout);
  d->refresh_timeout = g_timeout_add(1500, _studio_deferred_refresh, d);
}

/**
 * @brief Refresh the display when the history of the shown image changes
 * (e.g. styles re-applied manually from the Style module).
 */
static void _studio_history_changed_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;
  if(d->imgid <= UNKNOWN_IMAGE) return;

  // The style was written straight to DB (common/styles.c uses a throwaway dev, never
  // d->dev), so d->dev's in-memory history/pipeline are now stale for the displayed
  // image. Reload and reprocess them here so the scopes/color-picker samples — which
  // read from d->dev's preview pipe, not from the fetcher's mipmap-based surface —
  // catch up at the same time as the center image. Skip dt_dev_history_gui_update():
  // it walks dev->iop expecting module GUIs, which this viewer never initializes.
  if(d->dev_loaded && dt_dev_get_global() == d->dev)
  {
    dt_dev_reload_history_items(d->dev, d->imgid);
    dt_dev_history_pixelpipe_update(d->dev, TRUE);
  }

  dt_view_image_surface_fetcher_invalidate(&d->fetcher, &d->surface);
  dt_control_queue_redraw_center();
}

void enter(dt_view_t *self)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  // Publish our develop so the scopes/navigation modules and the pixelpipe read
  // this viewer's pipeline while the atelier is active.
  d->saved_develop = dt_dev_get_global();
  dt_dev_set_global(d->dev);

  // The Scopes module binds its instance and refresh callback onto whichever
  // develop is active at application startup (always darkroom's, since that
  // develop is created first). Copy that binding onto our own develop.
  // The primary sample's readout widgets (numeric label + swatch) are
  // likewise created once at startup and wired to darkroom's specific
  // primary_sample struct: share that instance while we are active so the
  // readout displays here too (own_primary_sample is restored in leave()).
  const dt_view_t *const darkroom_view = dt_view_manager_get_global()->proxy.darkroom.view;
  if(!IS_NULL_PTR(darkroom_view) && !IS_NULL_PTR(darkroom_view->data))
  {
    const dt_develop_t *const darkroom_dev = (const dt_develop_t *)darkroom_view->data;
    d->dev->color_picker.histogram_module = darkroom_dev->color_picker.histogram_module;
    d->dev->color_picker.refresh_global_picker = darkroom_dev->color_picker.refresh_global_picker;
    if(!IS_NULL_PTR(darkroom_dev->color_picker.primary_sample))
      d->dev->color_picker.primary_sample = darkroom_dev->color_picker.primary_sample;
    // The Scopes panel's picker toggle button is the same physical widget
    // regardless of the active view, wired at startup to darkroom_dev's
    // dt_iop_color_picker_t. _refresh_active_picker() (gui/color_picker_proxy.c)
    // bails out unconditionally when dev->color_picker.picker is NULL — without
    // this, our own dev never resamples on pipe-finished/cacheline-ready events,
    // so the primary sample keeps showing whatever value darkroom last computed
    // instead of the image displayed here.
    d->dev->color_picker.picker = darkroom_dev->color_picker.picker;
  }
  dt_iop_color_picker_init();

  dt_view_active_images_reset(FALSE);

  dt_ui_panel_show(dt_gui_get_ui(), DT_UI_PANEL_LEFT, TRUE, TRUE);
  dt_ui_panel_show(dt_gui_get_ui(), DT_UI_PANEL_RIGHT, FALSE, TRUE);
  dt_ui_panel_show(dt_gui_get_ui(), DT_UI_PANEL_BOTTOM, TRUE, TRUE);

  // Attach shortcuts
  dt_accels_connect_accels(dt_gui_get_accels());
  dt_accels_connect_active_group(dt_gui_get_accels(), "lighttable");

  gtk_widget_show(dt_gui_center_widget());
  dt_thumbtable_show(dt_gui_get_ui()->thumbtable_filmstrip);
  dt_thumbtable_update_parent(dt_gui_get_ui()->thumbtable_filmstrip);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                                  G_CALLBACK(_studio_filmstrip_activate_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_IMAGE_IMPORT,
                                  G_CALLBACK(_studio_image_imported_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                                  G_CALLBACK(_studio_history_changed_callback), self);

  dt_collection_update_query(dt_collection_get_global(), DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);

  // Start from the current selection when there is one.
  const int32_t imgid = dt_selection_get_first_id(dt_selection_get_global());
  if(imgid > UNKNOWN_IMAGE) _studio_set_image(d, imgid);

  g_idle_add((GSourceFunc)dt_thumbtable_scroll_to_selection, dt_gui_get_ui()->thumbtable_filmstrip);
  dt_gui_refocus_center();
}

void leave(dt_view_t *self)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_studio_filmstrip_activate_callback),
                                     (gpointer)self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_studio_image_imported_callback),
                                     (gpointer)self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_studio_history_changed_callback),
                                     (gpointer)self);

  if(d->refresh_timeout)
  {
    g_source_remove(d->refresh_timeout);
    d->refresh_timeout = 0;
  }

  // Release the color picker while our develop is still the global one: both
  // read/write darktable.develop directly, not a passed-in pointer.
  dt_iop_color_picker_cleanup();
  if(d->dev->color_picker.picker)
    dt_iop_color_picker_reset(d->dev->color_picker.picker->module, FALSE);
  d->picker_dragging_box = FALSE;

  // Stop sharing darkroom's primary sample instance (see enter()): our own
  // develop must own a real, private instance again before it can be freed.
  d->dev->color_picker.primary_sample = d->own_primary_sample;

  // Tear down the pipeline while our develop is still the global one, then
  // restore the previous global develop pointer.
  _studio_dev_teardown(d);
  dt_dev_set_global(d->saved_develop);
  // Force a fresh setup on the next enter even if the same image is shown.
  d->imgid = UNKNOWN_IMAGE;

  dt_view_image_surface_fetcher_invalidate(&d->fetcher, &d->surface);
  d->panning = FALSE;

  dt_accels_disconnect_active_group(dt_gui_get_accels());

  dt_thumbtable_hide(dt_gui_get_ui()->thumbtable_filmstrip);
  dt_view_active_images_reset(FALSE);
}

void configure(dt_view_t *self, int width, int height)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  // Only the active view owns a valid center allocation.
  if(dt_view_manager_get_current_view(dt_view_manager_get_global()) != self) return;

  d->dev_width = width;
  d->dev_height = height;

  // Re-size the running pipelines to the new center allocation.
  if(d->dev_loaded) _studio_configure_dev_roi(d, width, height);
}

void reset(dt_view_t *self)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;
  _studio_set_image(d, UNKNOWN_IMAGE);
}

/**
 * @brief Clamp the pan offsets so the viewport never leaves the surface.
 */
static void _studio_clamp_pan(dt_studio_capture_t *d, const double logical_width, const double logical_height)
{
  const double max_x = MAX(0.0, logical_width - d->width);
  const double max_y = MAX(0.0, logical_height - d->height);
  d->pan_x = CLAMP(d->pan_x, 0.0, max_x);
  d->pan_y = CLAMP(d->pan_y, 0.0, max_y);
}

/**
 * @brief Compute where the displayed surface currently sits in widget space.
 *
 * Mirrors the placement expose() paints with: fit-to-window centers the
 * surface, 100% pans it by -pan_x/-pan_y (or centers it when it fits).
 * Shared by expose(), the zoom-anchor computation and the color picker
 * coordinate mapping so all three agree on the same on-screen geometry.
 *
 * @return FALSE when there is no surface to place yet.
 */
static gboolean _studio_surface_geometry(const dt_studio_capture_t *d, double *tr_x, double *tr_y,
                                         double *logical_width, double *logical_height)
{
  if(IS_NULL_PTR(d->surface)) return FALSE;

  const int surface_width = cairo_image_surface_get_width(d->surface);
  const int surface_height = cairo_image_surface_get_height(d->surface);
  double sx = 1.0, sy = 1.0;
  cairo_surface_get_device_scale(d->surface, &sx, &sy);
  *logical_width = surface_width / sx;
  *logical_height = surface_height / sy;

  if(d->zoom == DT_THUMBTABLE_ZOOM_FIT)
  {
    *tr_x = (d->width < *logical_width) ? 0.0 : (d->width - *logical_width) * .5;
    *tr_y = (d->height < *logical_height) ? 0.0 : (d->height - *logical_height) * .5;
  }
  else
  {
    *tr_x = (*logical_width <= d->width) ? (d->width - *logical_width) * .5 : -d->pan_x;
    *tr_y = (*logical_height <= d->height) ? (d->height - *logical_height) * .5 : -d->pan_y;
  }
  return TRUE;
}

/**
 * @brief Convert a widget-space point to a normalized [0,1] image point.
 *
 * The result is normalized over the displayed surface, which shares the same
 * crop/orientation as the viewer's own develop pipeline: this is therefore a
 * valid "image norm" point for dt_lib_colorpicker_set_point/set_box_area,
 * regardless of the surface's own pixel resolution (fit thumbnail vs 100%).
 */
static gboolean _studio_widget_to_image_norm(const dt_studio_capture_t *d, const double x, const double y,
                                             float point[2])
{
  double tr_x = 0.0, tr_y = 0.0, logical_width = 0.0, logical_height = 0.0;
  if(!_studio_surface_geometry(d, &tr_x, &tr_y, &logical_width, &logical_height)) return FALSE;
  if(logical_width <= 0.0 || logical_height <= 0.0) return FALSE;

  point[0] = CLAMP((float)((x - tr_x) / logical_width), 0.0f, 1.0f);
  point[1] = CLAMP((float)((y - tr_y) / logical_height), 0.0f, 1.0f);
  return TRUE;
}

/**
 * @brief Inverse of _studio_widget_to_image_norm(), for drawing the picker overlay.
 */
static gboolean _studio_image_norm_to_widget(const dt_studio_capture_t *d, const float point[2], double *x,
                                             double *y)
{
  double tr_x = 0.0, tr_y = 0.0, logical_width = 0.0, logical_height = 0.0;
  if(!_studio_surface_geometry(d, &tr_x, &tr_y, &logical_width, &logical_height)) return FALSE;

  *x = tr_x + point[0] * logical_width;
  *y = tr_y + point[1] * logical_height;
  return TRUE;
}

/**
 * @brief Draw one point or box color picker sample, matching darkroom's
 * _darkroom_pickers_draw visual style: a dark 3px outline under a lighter
 * 1px stroke (2x for the selected sample), corner handles / a circle handle
 * on the primary sample, dashing on non-primary/non-selected boxes, and a
 * filled swatch showing the picked color.
 *
 * Darkroom scales these constants by the darkroom zoom level so they stay a
 * constant LOGICAL pixel size on screen; our surface is drawn directly in
 * widget space, so plain device-pixel constants already achieve the same
 * "constant apparent size" effect without needing a zoom-scale factor.
 */
static void _studio_draw_one_sample(dt_studio_capture_t *d, cairo_t *cr, dt_colorpicker_sample_t *sample,
                                    const gboolean is_primary_sample, const gboolean selected)
{
  const double line_scale = selected ? 2.0 : 1.0;

  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    float image_box[4] = { sample->box[0], sample->box[1], sample->box[2], sample->box[3] };
    dt_dev_coordinates_raw_norm_to_image_norm(d->dev, image_box, 2);
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    if(!_studio_image_norm_to_widget(d, &image_box[0], &x0, &y0)
       || !_studio_image_norm_to_widget(d, &image_box[2], &x1, &y1))
      return;

    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    if(is_primary_sample)
    {
      const double hw = 5.0;
      cairo_rectangle(cr, x0 - hw, y0 - hw, 2. * hw, 2. * hw);
      cairo_rectangle(cr, x0 - hw, y1 - hw, 2. * hw, 2. * hw);
      cairo_rectangle(cr, x1 - hw, y0 - hw, 2. * hw, 2. * hw);
      cairo_rectangle(cr, x1 - hw, y1 - hw, 2. * hw, 2. * hw);
    }

    cairo_set_line_width(cr, 3.0 * line_scale);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
    cairo_stroke_preserve(cr);

    const gboolean draw_dashed = !is_primary_sample && !selected;
    const double dashes[1] = { 4.0 };
    cairo_set_line_width(cr, line_scale);
    cairo_set_dash(cr, dashes, draw_dashed ? 1 : 0, 0.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0.0);
    // Darkroom does not fill a swatch on-canvas for box samples either: their
    // color readout only shows in the Scopes panel's live sample list.
  }
  else
  {
    float image_point[2] = { sample->point[0], sample->point[1] };
    dt_dev_coordinates_raw_norm_to_image_norm(d->dev, image_point, 1);
    double px = 0.0, py = 0.0;
    if(!_studio_image_norm_to_widget(d, image_point, &px, &py)) return;

    // Darkroom's constant-size fallback (min_half_px_device): the zoom-scaled
    // half_px it computes almost always bottoms out at this value in practice.
    const double half_px = 4.0;
    double crosshair = (is_primary_sample ? 4.0 : 5.0) * half_px;
    if(selected) crosshair *= 2.0;

    if(is_primary_sample) cairo_arc(cr, px, py, crosshair, 0., 2. * M_PI);
    cairo_move_to(cr, px - crosshair, py);
    cairo_line_to(cr, px + crosshair, py);
    cairo_move_to(cr, px, py - crosshair);
    cairo_line_to(cr, px, py + crosshair);

    cairo_set_line_width(cr, 3.0 * line_scale);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
    cairo_stroke_preserve(cr);

    cairo_set_line_width(cr, line_scale);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);
    cairo_stroke(cr);

    // Darkroom fills a rectangle here only when zoomed in enough that half_px
    // reflects an actual preview-pixel size; at our constant device-pixel
    // half_px that case does not apply, so the swatch is always a circle
    // (radius doubled for the selected sample), matching darkroom's own
    // fallback behavior at typical (non-extreme) zoom levels.
    cairo_arc(cr, px, py, selected ? half_px * 2.0 : half_px, 0., 2. * M_PI);
    cairo_set_source_rgba(cr, sample->swatch.red, sample->swatch.green, sample->swatch.blue,
                          sample->swatch.alpha);
    cairo_fill(cr);
  }
}

/**
 * @brief Draw the live samples list, then the active picker, matching
 * darkroom's two-pass _darkroom_pickers_draw rendering and its visibility
 * rules (live samples show when "display all" is on, or a single sample is
 * hover-highlighted from the Scopes panel list).
 */
static void _studio_draw_pickers(dt_studio_capture_t *d, cairo_t *cr)
{
  dt_develop_t *dev = d->dev;
  dt_colorpicker_sample_t *const selected_sample = dev->color_picker.selected_sample;

  cairo_save(cr);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);

  if(dev->color_picker.samples
     && (dev->color_picker.display_samples
         || (selected_sample && selected_sample != dev->color_picker.primary_sample)))
  {
    const gboolean only_selected = selected_sample && !dev->color_picker.display_samples;
    for(GSList *l = dev->color_picker.samples; l; l = g_slist_next(l))
    {
      dt_colorpicker_sample_t *sample = l->data;
      if(only_selected && sample != selected_sample) continue;
      _studio_draw_one_sample(d, cr, sample, FALSE, sample == selected_sample);
    }
  }

  if(dt_iop_color_picker_is_visible(dev) && !IS_NULL_PTR(dev->color_picker.primary_sample))
    _studio_draw_one_sample(d, cr, dev->color_picker.primary_sample, TRUE,
                            dev->color_picker.primary_sample == selected_sample);

  cairo_restore(cr);
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;
  d->width = width;
  d->height = height;

  // The view's configure() callback only fires on window resizes, not on view
  // entry. Size the running pipelines here so the preview pipe (the scopes'
  // source) always gets a valid ROI, and follow later center resizes.
  if(d->dev_loaded && (d->dev_width != width || d->dev_height != height))
  {
    d->dev_width = width;
    d->dev_height = height;
    _studio_configure_dev_roi(d, width, height);
  }

  dt_aligned_pixel_t bg_color = { 0.0f };
  dt_dev_get_background_color(d->dev, bg_color);
  cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
  cairo_paint(cr);

  if(d->imgid <= UNKNOWN_IMAGE) return;

  // Always fetch/refresh the mipmap surface first: _studio_widget_to_image_norm() (the color
  // picker's click-to-image-point mapping, in button_pressed/mouse_moved) keys off its
  // dimensions via _studio_surface_geometry(), so both must stay valid and current even on
  // frames where the live backbuf below ends up being what actually gets painted. Cheap: this
  // is a cached async fetch, not a re-decode, on frames where nothing relevant changed.
  const dt_view_surface_value_t res
      = dt_view_image_get_surface_async(&d->fetcher, d->imgid, MAX(2, width), MAX(2, height), &d->surface,
                                        dt_gui_center_widget(), d->zoom);
  if(res != DT_VIEW_SURFACE_OK || IS_NULL_PTR(d->surface))
  {
    dt_control_draw_busy_msg(cr, width, height);
    dt_dev_draw_profile_mode_label(cr, height);
    return;
  }

  double tr_x = 0.0, tr_y = 0.0, logical_width = 0.0, logical_height = 0.0;
  // First 100% frame after a zoom request: place the image point that was
  // under the pointer back under the pointer. Needs the surface's logical
  // size, so peek it once before the shared geometry helper applies pan/clamp.
  if(d->zoom != DT_THUMBTABLE_ZOOM_FIT && d->anchor_pending)
  {
    const int surface_width = cairo_image_surface_get_width(d->surface);
    const int surface_height = cairo_image_surface_get_height(d->surface);
    double sx = 1.0, sy = 1.0;
    cairo_surface_get_device_scale(d->surface, &sx, &sy);
    d->pan_x = d->anchor_rel_x * (surface_width / sx) - d->anchor_px;
    d->pan_y = d->anchor_rel_y * (surface_height / sy) - d->anchor_py;
    d->anchor_pending = FALSE;
  }
  _studio_surface_geometry(d, &tr_x, &tr_y, &logical_width, &logical_height);
  if(d->zoom != DT_THUMBTABLE_ZOOM_FIT) _studio_clamp_pan(d, logical_width, logical_height);

  // Prefer the live main pipe's backbuf over the plain mipmap thumbnail: it is what actually
  // bakes in ISO 12646/overexposed/raw overexposed/softproof/gamut, none of which the mipmap
  // fetcher above is aware of. Only meaningful at FIT: d->dev->roi never reflects zoom/pan (see
  // _studio_configure_dev_roi), so at 100% this pipe is always framed as "fit", not "100%" -
  // trying to show it there would just display the wrong framing under the wrong label. Falls
  // back to the mipmap surface (translated/sized by the geometry above) whenever the live
  // backbuf is not ready yet (cold start, image just switched) or zoom is 100%.
  gboolean drew_live = FALSE;
  if(d->zoom == DT_THUMBTABLE_ZOOM_FIT
     && dt_dev_lock_pipe_surface(d->dev, d->dev->pipe, &d->main_locked, &d->main_wait, "studio-capture-main", TRUE)
     && d->main_locked.surface)
  {
    // dt_dev_render_locked_surface() translates cr directly with no save/restore of its own:
    // darkroom always calls it on a throwaway intermediate context it discards right after, but
    // this view draws everything (including the guides below) on the same cr, so the translate
    // must not leak past this call.
    cairo_save(cr);
    drew_live = dt_dev_render_locked_surface(cr, d->dev, &d->main_locked, width, height,
                                             dt_dev_viewport_border_size(d->dev), bg_color);
    cairo_restore(cr);
  }

  if(!drew_live)
  {
    cairo_save(cr);
    cairo_translate(cr, tr_x, tr_y);
    if(d->zoom == DT_THUMBTABLE_ZOOM_FIT && d->dev->iso_12646.enabled)
      dt_dev_draw_iso12646_border(cr, logical_width, logical_height, dt_dev_viewport_border_size(d->dev));
    cairo_set_source_surface(cr, d->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), dt_widget_image_filter());
    cairo_rectangle(cr, 0, 0, logical_width, logical_height);
    cairo_fill(cr);
    cairo_restore(cr);
  }

  // Guide lines: the toggle button (darktable.view_manager->guides_toggle, wired up in
  // darkroom.c's gui_init) is already shared into this view's toolbox, but its state and
  // dt_guides_draw() itself read only global conf, independent of any dev - only the
  // clip/transform setup below is dev-driven. Only meaningful at FIT, same reasoning as the
  // live backbuf above: d->dev->roi always describes "fit", never this view's own 100%/pan.
  if(d->zoom == DT_THUMBTABLE_ZOOM_FIT)
  {
    const float wd = dt_dev_roi_request_preview_width(d->dev);
    const float ht = dt_dev_roi_request_preview_height(d->dev);
    const float scaling = dt_dev_get_overlay_scale(d->dev);

    cairo_save(cr);
    // don't draw guides on image margins
    dt_dev_clip_roi(d->dev, cr, width, height);
    // place origin at top-left corner of image
    dt_dev_rescale_roi(d->dev, cr, width, height);

    dt_guides_draw(cr, 0, 0, wd, ht, scaling);
    cairo_restore(cr);
  }

  _studio_draw_pickers(d, cr);
  dt_dev_draw_profile_mode_label(cr, height);
}

/**
 * @brief Toggle between fit and 100%, anchoring the zoom on the pointer.
 */
static void _studio_toggle_zoom(dt_studio_capture_t *d, const double x, const double y)
{
  if(d->imgid <= UNKNOWN_IMAGE) return;

  if(d->zoom == DT_THUMBTABLE_ZOOM_FIT)
  {
    d->zoom = DT_THUMBTABLE_ZOOM_FULL;

    // Convert the pointer position to a relative image point using the
    // currently displayed fit surface.
    d->anchor_rel_x = 0.5;
    d->anchor_rel_y = 0.5;
    double tr_x = 0.0, tr_y = 0.0, logical_width = 0.0, logical_height = 0.0;
    if(_studio_surface_geometry(d, &tr_x, &tr_y, &logical_width, &logical_height) && logical_width > 0.0
       && logical_height > 0.0)
    {
      d->anchor_rel_x = CLAMP((x - tr_x) / logical_width, 0.0, 1.0);
      d->anchor_rel_y = CLAMP((y - tr_y) / logical_height, 0.0, 1.0);
    }
    d->anchor_px = x;
    d->anchor_py = y;
    d->anchor_pending = TRUE;
  }
  else
  {
    d->zoom = DT_THUMBTABLE_ZOOM_FIT;
    d->anchor_pending = FALSE;
  }

  d->pan_x = d->pan_y = 0.0;
  d->panning = FALSE;
  dt_control_queue_redraw_center();
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  if(dt_iop_color_picker_is_visible(d->dev) && dt_control_get_global()->button_down
     && dt_control_get_global()->button_down_which == 1)
  {
    if(d->picker_dragging_box)
    {
      float point[2] = { 0.0f };
      if(_studio_widget_to_image_norm(d, x, y, point))
      {
        const dt_boundingbox_t box = { MIN(d->picker_box_anchor[0], point[0]),
                                       MIN(d->picker_box_anchor[1], point[1]),
                                       MAX(d->picker_box_anchor[0], point[0]),
                                       MAX(d->picker_box_anchor[1], point[1]) };
        dt_lib_colorpicker_set_box_area(dt_lib_get_global(), box);
        dt_control_queue_redraw_center();
      }
    }
    else if(d->dev->color_picker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      // Point mode: the picker follows the pointer for as long as the button
      // stays down, matching darkroom's mouse_moved.
      float point[2] = { 0.0f };
      if(_studio_widget_to_image_norm(d, x, y, point))
      {
        dt_lib_colorpicker_set_point(dt_lib_get_global(), point);
        dt_control_queue_redraw_center();
      }
    }
    return;
  }

  if(!d->panning) return;

  d->pan_x = d->pan_start_x - (x - d->drag_start_x);
  d->pan_y = d->pan_start_y - (y - d->drag_start_y);
  dt_control_queue_redraw_center();
}

/**
 * @brief Left-click handling while the color picker is active.
 *
 * Point vs. box mode is decided by sample->size, which the picker button's
 * own ctrl+click/right-click already sets (shared code in
 * gui/color_picker_proxy.c, unrelated to this handler). Mirrors darkroom's
 * button_pressed: clicking near an existing box corner grabs it (the
 * opposite corner becomes the drag anchor); clicking elsewhere starts a
 * fresh small default box centered on the click.
 */
static void _studio_picker_left_click(dt_studio_capture_t *d, const float point[2], const double logical_width,
                                      const double logical_height)
{
  dt_colorpicker_sample_t *const sample = d->dev->color_picker.primary_sample;

  if(sample->size != DT_LIB_COLORPICKER_SIZE_BOX)
  {
    d->picker_dragging_box = FALSE;
    dt_lib_colorpicker_set_point(dt_lib_get_global(), point);
    return;
  }

  float image_box[4] = { sample->box[0], sample->box[1], sample->box[2], sample->box[3] };
  dt_dev_coordinates_raw_norm_to_image_norm(d->dev, image_box, 2);

  // 6 widget pixels of corner-grab slop, converted to image-normalized units.
  const float handle_x = (logical_width > 0.0) ? (float)(6.0 / logical_width) : 0.02f;
  const float handle_y = (logical_height > 0.0) ? (float)(6.0 / logical_height) : 0.02f;

  gboolean on_corner = TRUE;
  float opposite[2] = { 0.0f };
  if(fabsf(point[0] - image_box[0]) <= handle_x) opposite[0] = image_box[2];
  else if(fabsf(point[0] - image_box[2]) <= handle_x) opposite[0] = image_box[0];
  else on_corner = FALSE;

  if(fabsf(point[1] - image_box[1]) <= handle_y) opposite[1] = image_box[3];
  else if(fabsf(point[1] - image_box[3]) <= handle_y) opposite[1] = image_box[1];
  else on_corner = FALSE;

  if(on_corner)
  {
    d->picker_box_anchor[0] = opposite[0];
    d->picker_box_anchor[1] = opposite[1];
  }
  else
  {
    // The default box is a small square, 1% of the image width, centered on the click.
    const float delta = 0.01f;
    d->picker_box_anchor[0] = point[0];
    d->picker_box_anchor[1] = point[1];
    const dt_boundingbox_t box = { fmaxf(0.0f, point[0] - delta), fmaxf(0.0f, point[1] - delta),
                                   fminf(1.0f, point[0] + delta), fminf(1.0f, point[1] + delta) };
    dt_lib_colorpicker_set_box_area(dt_lib_get_global(), box);
  }

  d->picker_dragging_box = TRUE;
  dt_control_change_cursor(GDK_FLEUR);
}

/**
 * @brief Right-click handling while the color picker is active: reuse an
 * overlapping live sample's geometry (mirrors darkroom's button_pressed
 * which==3 branch), or reset the box to a default centered area.
 */
static void _studio_picker_right_click(dt_studio_capture_t *d, const float point[2], const double logical_width,
                                       const double logical_height)
{
  dt_develop_t *const dev = d->dev;
  dt_colorpicker_sample_t *const sample = dev->color_picker.primary_sample;

  if(dev->color_picker.display_samples && !IS_NULL_PTR(dev->color_picker.picker))
  {
    const float slop_x = (logical_width > 0.0) ? (float)(20.0 / logical_width) : 0.05f;
    const float slop_y = (logical_height > 0.0) ? (float)(20.0 / logical_height) : 0.05f;

    for(GSList *l = dev->color_picker.samples; l; l = g_slist_next(l))
    {
      dt_colorpicker_sample_t *live_sample = l->data;
      if(live_sample->size == DT_LIB_COLORPICKER_SIZE_BOX && sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      {
        float live_box[4] = { live_sample->box[0], live_sample->box[1], live_sample->box[2],
                              live_sample->box[3] };
        dt_dev_coordinates_raw_norm_to_image_norm(dev, live_box, 2);
        if(point[0] < live_box[0] || point[0] > live_box[2] || point[1] < live_box[1] || point[1] > live_box[3])
          continue;
        dt_lib_colorpicker_set_box_area(dt_lib_get_global(), live_box);
        return;
      }
      else if(live_sample->size == DT_LIB_COLORPICKER_SIZE_POINT && sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      {
        float live_point[2] = { live_sample->point[0], live_sample->point[1] };
        dt_dev_coordinates_raw_norm_to_image_norm(dev, live_point, 1);
        if(fabsf(point[0] - live_point[0]) > slop_x || fabsf(point[1] - live_point[1]) > slop_y) continue;
        dt_lib_colorpicker_set_point(dt_lib_get_global(), live_point);
        return;
      }
    }
  }

  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    const dt_boundingbox_t reset = { 0.01f, 0.01f, 0.99f, 0.99f };
    dt_lib_colorpicker_set_box_area(dt_lib_get_global(), reset);
  }
}

int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  // The color picker owns the center view while active: it takes precedence
  // over zoom toggling and panning, exactly like darkroom.
  if(dt_iop_color_picker_is_visible(d->dev) && (which == 1 || which == 3))
  {
    float point[2] = { 0.0f };
    double tr_x = 0.0, tr_y = 0.0, logical_width = 0.0, logical_height = 0.0;
    _studio_surface_geometry(d, &tr_x, &tr_y, &logical_width, &logical_height);

    if(_studio_widget_to_image_norm(d, x, y, point))
    {
      if(which == 1)
        _studio_picker_left_click(d, point, logical_width, logical_height);
      else
        _studio_picker_right_click(d, point, logical_width, logical_height);
      dt_control_queue_redraw_center();
    }
    return 1;
  }

  if(which == 2 || (which == 1 && type == GDK_2BUTTON_PRESS))
  {
    _studio_toggle_zoom(d, x, y);
    return 1;
  }

  if(which == 1 && d->zoom == DT_THUMBTABLE_ZOOM_FULL)
  {
    d->panning = TRUE;
    d->drag_start_x = x;
    d->drag_start_y = y;
    d->pan_start_x = d->pan_x;
    d->pan_start_y = d->pan_y;
    dt_control_change_cursor(GDK_HAND1);
    return 1;
  }

  return 0;
}

int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  if(dt_iop_color_picker_is_visible(d->dev) && which == 1)
  {
    if(d->picker_dragging_box) dt_control_change_cursor(GDK_LEFT_PTR);
    d->picker_dragging_box = FALSE;
    dt_control_queue_redraw_center();
    return 1;
  }

  if(d->panning && which == 1)
  {
    d->panning = FALSE;
    dt_control_change_cursor(GDK_LEFT_PTR);
    return 1;
  }
  return 0;
}

int scrolled(dt_view_t *self, double x, double y, int up, int state, int delta_y)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  if(up && d->zoom == DT_THUMBTABLE_ZOOM_FIT)
  {
    _studio_toggle_zoom(d, x, y);
    return 1;
  }
  if(!up && d->zoom == DT_THUMBTABLE_ZOOM_FULL)
  {
    _studio_toggle_zoom(d, x, y);
    return 1;
  }
  return 0;
}

int key_pressed(dt_view_t *self, GdkEventKey *event)
{
  dt_studio_capture_t *d = (dt_studio_capture_t *)self->data;

  const gboolean shift = dt_modifier_is(event->state, GDK_SHIFT_MASK);
  const gboolean ctrl = dt_modifier_is(event->state, GDK_CONTROL_MASK);
  const gboolean ctrl_any = dt_modifiers_include(event->state, GDK_CONTROL_MASK);
  guint key = dt_keys_mainpad_alternatives(event->keyval);

  // Studio Capture's visible surface is driven by d->zoom/pan_x/pan_y (see
  // expose()), not by d->dev->roi: that develop only feeds the Scopes module
  // in the background and never reaches the screen. Panning or zooming it
  // has no visible effect, so all navigation below must go through the same
  // fields the mouse handlers (button_pressed/mouse_moved/scrolled) use.
  if(ctrl_any && (key == GDK_KEY_plus || key == GDK_KEY_minus))
  {
    if((key == GDK_KEY_plus) == (d->zoom == DT_THUMBTABLE_ZOOM_FIT))
      _studio_toggle_zoom(d, d->width / 2.0, d->height / 2.0);
    return 1;
  }

  const double multiplier = shift ? 4.0 : ctrl ? 0.5 : 1.0;
  const double step = 10.0 * multiplier;

  switch(key)
  {
    case GDK_KEY_Return:
    {
      if(d->imgid > UNKNOWN_IMAGE)
      {
        // _studio_set_image() already synced the global selection to d->imgid.
        dt_view_manager_switch(dt_view_manager_get_global(), "darkroom");
      }
      else
        dt_control_log(_("No image to open in darkroom."));
      return 1;
    }
    case GDK_KEY_Up:
    {
      if(d->zoom != DT_THUMBTABLE_ZOOM_FULL) return 0;
      d->pan_y -= step;
      dt_control_queue_redraw_center();
      return 1;
    }
    case GDK_KEY_Down:
    {
      if(d->zoom != DT_THUMBTABLE_ZOOM_FULL) return 0;
      d->pan_y += step;
      dt_control_queue_redraw_center();
      return 1;
    }
    case GDK_KEY_Left:
    {
      if(d->zoom != DT_THUMBTABLE_ZOOM_FULL) return 0;
      d->pan_x -= step;
      dt_control_queue_redraw_center();
      return 1;
    }
    case GDK_KEY_Right:
    {
      if(d->zoom != DT_THUMBTABLE_ZOOM_FULL) return 0;
      d->pan_x += step;
      dt_control_queue_redraw_center();
      return 1;
    }
    case GDK_KEY_Escape:
    {
      dt_ctl_switch_mode_to("lighttable");
      return TRUE;
    }
  }

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
