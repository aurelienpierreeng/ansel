/*
    This file is part of Ansel,
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

#include "develop/imageop_gui.h"
#include "widgets/widget_settings.h"
#include "develop/dev_history_gui.h"

#include "develop/blend_gui.h"
#include "develop/dev_history.h"
#include "develop/gui_throttle.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"
#include "system/macros.h"

#include <gtk/gtk.h>

/* Undoing an edit also undoes what the user was LOOKING at: the recorded mask-edit view
 * for the focused module. The data half (dt_masks_set_edit_mode, request_mask_display) is
 * restored by the engine; this pokes the blending panel's widgets to match. */
static void _undo_restore_gui(dt_develop_t *dev, const int mask_edit_mode,
                              const int request_mask_display)
{
  (void)mask_edit_mode;   // consumed by the engine's dt_masks_set_edit_mode() call

  dt_iop_gui_update_blendif(dev->gui_module);
  dt_iop_gui_blend_data_t *bd = dev->gui_module && dev->gui_module->gui
                                  ? (dt_iop_gui_blend_data_t *)dev->gui_module->gui->blend_data
                                  : NULL;
  if(bd)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask),
                                 request_mask_display == DT_DEV_PIXELPIPE_DISPLAY_MASK);
}

void dt_dev_history_gui_update(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;

  // Match the live module instances to the reloaded history before touching GTK.
  // This loop may remove obsolete instances or expose instances newly created
  // while reading a style/history from the database.
  GList *removed = NULL;
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_history_refresh_nodes_ext(dev, &dev->iop, dev->history, &removed);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  dt_gui_freeze_begin();

  // Destroy the widgets of the instances the reconciliation removed -- AFTER the lock,
  // which is the point of the removed-list protocol (see dev_history.h); the modules are
  // parked in dev->alliop and already unlinked, so nothing else touches them.
  for(GList *l = removed; l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    if(dev->gui_module == mod) dt_iop_request_focus(NULL);
    if(!dt_iop_is_hidden(mod) && mod->gui && mod->gui->expander)
    {
      // hide first to avoid a burst of gtk critical warnings from the live container
      gtk_widget_hide(mod->gui->expander);
      // frees mod->gui and destroys the whole expander/header/widget tree itself
      dt_iop_gui_cleanup_module(mod);
    }
  }
  g_list_free(removed);

  for(GList *module = g_list_first(dev->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(module->data);

    // History reload is backend-only and creates new multi-instances without
    // GTK state. Attach every missing GUI here, after releasing history_mutex,
    // so styles and global history actions expose their complete module set.
    if(!dt_iop_is_hidden(mod) && (IS_NULL_PTR(mod->gui) || IS_NULL_PTR(mod->gui->expander)))
    {
      // a backend-created instance has no gui struct at all -- that IS "never initialised"
      if(IS_NULL_PTR(mod->gui) || IS_NULL_PTR(mod->gui->widget)) dt_iop_gui_init(mod);
      dt_iop_gui_set_expander(mod);
    }

    // Parameters, enabled state, headers and blending controls may all have
    // changed, therefore refresh every module rather than only history entries.
    dt_iop_gui_update(mod);
  }

  dt_dev_masks_list_change(dev);
  dt_gui_freeze_end();

  dt_dev_signal_modules_moved(dev);
}
/* ---------------------------------------------------------------------------------------
 * Throttling the history commit, at the one place every widget already goes through.
 *
 * A history commit is not cheap: it records an undo step, takes history_mutex as writer,
 * rehashes the whole history, writes the image cache, rebuilds the masks list, schedules a
 * DB+XMP write, and finally tells the pipes their history changed -- which also raises their
 * `shutdown` atomic, so the worker abandons the frame it is rendering. Running that per step
 * of a slider drag or a combobox scroll blocks the GUI thread often enough that the widget
 * cannot even repaint its own value, and aborts each render before it can finish.
 *
 * Widgets used to dodge that by deferring their own `value-changed` emission through
 * gui_throttle (bauhaus, and a copy of the same dance in nine curve modules). That put the
 * policy in every widget that wanted it and left every other widget without it. The commit
 * is deferred here instead, where all of them already arrive.
 *
 * TWO RULES, and the difference between them matters:
 *
 * 1. NOTHING IS MERGED. A tempting optimisation is to collapse pending requests into one
 *    carrying the union of their flags; do not. Drawn masks, raster masks, module
 *    enable/disable, the mask manager and plain parameter edits each commit differently, and
 *    any merging rule is somewhere to silently drop one of them. The queue is FIFO and every
 *    distinct request is kept, in order.
 *
 * 2. A REPEAT OF THE PENDING TAIL IS NOT A NEW REQUEST. If the last thing queued is already
 *    "commit `module`, enable=`enable`" and that is exactly what is being asked for again,
 *    there is nothing to add: the queued request reads module->params when it drains, so it
 *    necessarily commits the newest value. This is consecutive-duplicate suppression against
 *    the TAIL only -- never a search of the queue, never a combination of two different
 *    requests -- so ordering is exactly preserved. It is what keeps a 300-tick scroll from
 *    queueing 300 commits, which is the whole point: a transient intermediate value of an
 *    ongoing gesture is not a history event.
 *
 * This is a batching window, not a debounce: the timer is armed by the first request and is
 * NOT re-armed by the ones that follow, so a sustained drag keeps committing once per window
 * instead of freezing until the user stops moving.
 *
 * dt_dev_add_history_item_ext() is NOT throttled and never was. Anything that must land
 * before the next statement (bulk history loads, style application, image duplication)
 * already goes through it.
 *
 * GUI thread only, like the commit path it serves ("always called from GUI controls" below),
 * hence no lock -- same contract as gui_throttle's own queue.
 */

typedef struct dt_dev_pending_commit_t
{
  dt_develop_t *dev;
  dt_iop_module_t *module;
  gboolean enable;
} dt_dev_pending_commit_t;

static GQueue _pending_commits = G_QUEUE_INIT;
static guint _pending_commit_source = 0;


static gboolean _drain_pending_commits(gpointer user_data)
{
  (void)user_data;

  _pending_commit_source = 0;

  // Detach the queue before draining: a commit can trigger another one (a module reacting in
  // post_history_commit), which would push onto the queue we are iterating.
  GQueue ready = G_QUEUE_INIT;
  ready.head = _pending_commits.head;
  ready.tail = _pending_commits.tail;
  ready.length = _pending_commits.length;
  g_queue_init(&_pending_commits);

  while(!g_queue_is_empty(&ready))
  {
    dt_dev_pending_commit_t *request = (dt_dev_pending_commit_t *)g_queue_pop_head(&ready);
    dt_dev_history_commit_item_now(request->dev, request->module, request->enable);
    dt_free(request);
  }

  return G_SOURCE_REMOVE;
}

static void _queue_pending_commit(dt_develop_t *dev, dt_iop_module_t *module, const gboolean enable)
{
  // A zero timeout means the user turned throttling off in the preferences (and is also what
  // a headless run gets, since nothing ever sets one there). Same call, same place, just now.
  const guint timeout_ms = dt_gui_throttle_get_timeout_ms();
  if(timeout_ms == 0)
  {
    dt_dev_history_commit_item_now(dev, module, enable);
    return;
  }

  // Rule 2 above: a repeat of what is already queued at the tail adds nothing.
  const dt_dev_pending_commit_t *tail = (const dt_dev_pending_commit_t *)_pending_commits.tail
                                        ? (const dt_dev_pending_commit_t *)_pending_commits.tail->data
                                        : NULL;
  if(!IS_NULL_PTR(tail) && tail->dev == dev && tail->module == module && tail->enable == enable) return;

  dt_dev_pending_commit_t *queued = (dt_dev_pending_commit_t *)calloc(1, sizeof(*queued));
  if(IS_NULL_PTR(queued))
  {
    // Out of memory for a 24-byte record: commit now rather than lose the edit.
    dt_dev_history_commit_item_now(dev, module, enable);
    return;
  }

  queued->dev = dev;
  queued->module = module;
  queued->enable = enable;
  g_queue_push_tail(&_pending_commits, queued);

  if(!_pending_commit_source)
    _pending_commit_source = g_timeout_add(timeout_ms, _drain_pending_commits, NULL);
}

void dt_dev_history_flush_pending_commits(dt_develop_t *dev)
{
  // Run them, do not drop them: a pending request IS the user's last edit, and dropping it
  // would lose the value they left a slider on. Callers invoke this while `dev` is still
  // whole -- before any pipe node, iop or history teardown -- so committing here is safe,
  // and it is the only moment at which it still is. A request draining after teardown is the
  // race CLAUDE.md documents, which corrupts the heap and crashes somewhere unrelated.
  if(_pending_commit_source)
  {
    g_source_remove(_pending_commit_source);
    _pending_commit_source = 0;
  }

  GList *iter = _pending_commits.head;
  while(iter)
  {
    GList *next = g_list_next(iter);
    dt_dev_pending_commit_t *request = (dt_dev_pending_commit_t *)iter->data;
    if(IS_NULL_PTR(dev) || request->dev == dev)
    {
      g_queue_delete_link(&_pending_commits, iter);
      dt_dev_history_commit_item_now(request->dev, request->module, request->enable);
      dt_free(request);
    }
    iter = next;
  }

  // Something else's requests may remain (another dev): give them their timer back.
  if(!g_queue_is_empty(&_pending_commits))
  {
    const guint timeout_ms = dt_gui_throttle_get_timeout_ms();
    if(timeout_ms > 0) _pending_commit_source = g_timeout_add(timeout_ms, _drain_pending_commits, NULL);
    else _drain_pending_commits(NULL);
  }
}

void dt_dev_history_drop_pending_commits(dt_develop_t *dev)
{
  // Last-resort counterpart to the flush, for a `dev` that is already too far gone to commit
  // to. Nothing should normally be left here by the time this runs.
  GList *iter = _pending_commits.head;
  while(iter)
  {
    GList *next = g_list_next(iter);
    dt_dev_pending_commit_t *request = (dt_dev_pending_commit_t *)iter->data;
    if(IS_NULL_PTR(dev) || request->dev == dev)
    {
      g_queue_delete_link(&_pending_commits, iter);
      dt_free(request);
    }
    iter = next;
  }

  if(_pending_commit_source && g_queue_is_empty(&_pending_commits))
  {
    g_source_remove(_pending_commit_source);
    _pending_commit_source = 0;
  }
}


// The next 2 functions are always called from GUI controls setting parameters
// This is why they directly start a pipeline recompute.
// Otherwise, please keep GUI and pipeline fully separated.

void dt_dev_add_history_item_real(dt_develop_t *dev, dt_iop_module_t *module, gboolean enable, gboolean redraw)
{
  (void)redraw;
  _queue_pending_commit(dev, module, enable);
}

/* After each immediate commit: keep the viewport geometry and the enable toggle honest. */
static void _commit_gui(dt_develop_t *dev, dt_iop_module_t *module)
{
  // If module params change the geometry of the ROI, update immediately so we avoid
  // drawing glitches.
  if(module->modify_roi_in || module->modify_roi_out)
    dt_dev_get_thumbnail_size(dev);

  // Changing a parameter of a disabled module enables it, so update the GUI toggle state
  // to reflect it -- frozen, so setting the state does not run its callbacks.
  dt_gui_freeze_begin();
  dt_iop_gui_set_enable_button(module);
  dt_gui_freeze_end();
}

void dt_dev_history_gui_init(void)
{
  dt_dev_history_set_undo_restore_gui_handler(_undo_restore_gui);
  dt_dev_history_set_commit_gui_handler(_commit_gui);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
