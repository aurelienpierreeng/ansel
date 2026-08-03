/*
    This file is part of darktable,
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2011, 2013 Pascal de Bruijn.
    Copyright (C) 2012-2013 parafin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014-2018 Tobias Ellinghaus.
    Copyright (C) 2013 johannes hanika.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014 Edouard Gomez.
    Copyright (C) 2014-2015 Jérémy Rosen.
    Copyright (C) 2014, 2020-2021 Pascal Obry.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2019, 2025 Aurélien PIERRE.
    Copyright (C) 2020 Marco.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Aldric Renaudin.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Nicolas Auffray.
    
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

#include "common/atomic.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/progress.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_backgroundjob_element_t
{
  GtkWidget *widget, *label, *progressbar, *hbox;
  // Proxy calls (updated/message_updated/cancellable/destroyed) run on arbitrary worker threads
  // and only ever schedule a same-named _*_gui_thread() callback to run later, on the GUI thread,
  // via g_main_context_invoke(). A job can finish (and get destroyed) while one of those callbacks
  // is still queued: without this refcount, _destroyed_gui_thread() would free `instance` right
  // out from under it, and the queued callback would touch freed GTK widgets on the next main-loop
  // iteration (Sentry issue 130394919: AccessViolation in gtk_label_set_text). Every scheduled
  // callback holds one reference, taken by _instance_ref() before g_main_context_invoke() and
  // dropped by _instance_unref() at the end of the callback; the struct is freed once the count
  // reaches zero, whichever callback that happens to be.
  dt_atomic_int refcount;
  // Set by _destroyed_gui_thread(), checked by _added_gui_thread(): a job can be destroyed before
  // its own _added_gui_thread() callback (which does the actual widget creation, see below) has
  // run, if both were scheduled in quick succession from a worker thread. Both callbacks only
  // ever run on the GUI thread, so plain gboolean read/write is safe without an atomic.
  gboolean destroyed;
} dt_lib_backgroundjob_element_t;

// Takes a reference on `instance` before scheduling a deferred GUI callback that touches it.
static dt_lib_backgroundjob_element_t *_instance_ref(dt_lib_backgroundjob_element_t *instance)
{
  if(!IS_NULL_PTR(instance)) dt_atomic_add_int(&instance->refcount, 1);
  return instance;
}

// Drops a reference taken by _instance_ref(); frees `instance` once the last reference is dropped.
static void _instance_unref(dt_lib_backgroundjob_element_t *instance)
{
  if(!IS_NULL_PTR(instance) && dt_atomic_sub_int(&instance->refcount, 1) == 1) dt_free(instance);
}

/* proxy functions */
static void *_lib_backgroundjobs_added(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message);
static void _lib_backgroundjobs_destroyed(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance);
static void _lib_backgroundjobs_cancellable(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                            dt_progress_t *progress);
static void _lib_backgroundjobs_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                        double value);
static void _lib_backgroundjobs_message_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                                const gchar *message);


const char *name(struct dt_lib_module_t *self)
{
  return _("background jobs");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

int position()
{
  return 1;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize base */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_no_show_all(self->widget, TRUE);

  /* setup proxy */
  dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);

  darktable.control->progress_system.proxy.module = self;
  darktable.control->progress_system.proxy.added = _lib_backgroundjobs_added;
  darktable.control->progress_system.proxy.destroyed = _lib_backgroundjobs_destroyed;
  darktable.control->progress_system.proxy.cancellable = _lib_backgroundjobs_cancellable;
  darktable.control->progress_system.proxy.updated = _lib_backgroundjobs_updated;
  darktable.control->progress_system.proxy.message_updated = _lib_backgroundjobs_message_updated;

  // iterate over darktable.control->progress_system.list and add everything that is already there and update
  // its gui_data!
  for(const GList *iter = darktable.control->progress_system.list; iter; iter = g_list_next(iter))
  {
    dt_progress_t *progress = (dt_progress_t *)iter->data;
    void *gui_data = dt_control_progress_get_gui_data(progress);
    dt_free(gui_data);
    gui_data = _lib_backgroundjobs_added(self, dt_control_progress_has_progress_bar(progress),
                                         dt_control_progress_get_message(progress));
    dt_control_progress_set_gui_data(progress, gui_data);
    if(dt_control_progress_cancellable(progress)) _lib_backgroundjobs_cancellable(self, gui_data, progress);
    _lib_backgroundjobs_updated(self, gui_data, dt_control_progress_get_progress(progress));
  }

  dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* lets kill proxy */
  dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);
  darktable.control->progress_system.proxy.module = NULL;
  darktable.control->progress_system.proxy.added = NULL;
  darktable.control->progress_system.proxy.destroyed = NULL;
  darktable.control->progress_system.proxy.cancellable = NULL;
  darktable.control->progress_system.proxy.updated = NULL;
  dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);
}

/** the proxy functions */

typedef struct _added_gui_thread_t
{
  GtkWidget *self_widget;
  dt_lib_backgroundjob_element_t *instance;
  gchar *message;
  gboolean has_progress_bar;
} _added_gui_thread_t;

static gboolean _added_gui_thread(gpointer user_data)
{
  _added_gui_thread_t *params = (_added_gui_thread_t *)user_data;
  dt_lib_backgroundjob_element_t *instance = params->instance;

  // The job may already have been destroyed (params->self, the proxy module, could even be gone
  // by now) if _lib_backgroundjobs_destroyed() ran first -- see the struct comment on `destroyed`.
  if(!instance->destroyed)
  {
    instance->widget = gtk_event_box_new();

    /* initialize the ui elements for job */
    gtk_widget_set_name(GTK_WIDGET(instance->widget), "background-job-eventbox");
    dt_gui_add_class(GTK_WIDGET(instance->widget), "dt_big_btn_canvas");
    GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING));
    instance->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
    gtk_container_add(GTK_CONTAINER(instance->widget), GTK_WIDGET(vbox));

    /* add job label */
    instance->label = gtk_label_new(params->message);
    gtk_widget_set_halign(instance->label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(instance->label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(instance->hbox), GTK_WIDGET(instance->label), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(instance->hbox), TRUE, TRUE, 0);

    /* use progressbar ? */
    if(params->has_progress_bar)
    {
      instance->progressbar = gtk_progress_bar_new();
      gtk_box_pack_start(GTK_BOX(vbox), instance->progressbar, TRUE, FALSE, 0);
    }

    /* lets show jobbox if its hidden */
    gtk_box_pack_start(GTK_BOX(params->self_widget), instance->widget, TRUE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(params->self_widget), instance->widget, 1);
    gtk_widget_show_all(instance->widget);
    gtk_widget_show(params->self_widget);
  }

  _instance_unref(instance);
  dt_free(params->message);
  dt_free(params);
  return FALSE;
}

static void *_lib_backgroundjobs_added(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message)
{
  // Only allocate `instance` here (no GTK call) and return it right away: dt_control_progress_create()
  // (control/progress.c) can call this proxy from any worker thread -- e.g. dt_folder_survey's
  // DT_JOB_QUEUE_SYSTEM_BG job auto-importing tethered captures calls dt_control_import() straight
  // from its own job-execution thread, unlike a manual File > Import which runs on the GUI thread.
  // GTK widget construction is not thread-safe; doing it here made the progress bar's very existence
  // depend on which thread happened to trigger the import, so it silently, intermittently failed to
  // appear for folder-survey auto-imports specifically. All actual widget creation now happens in
  // _added_gui_thread(), deferred like every other proxy callback in this file.
  dt_lib_backgroundjob_element_t *instance
      = (dt_lib_backgroundjob_element_t *)calloc(1, sizeof(dt_lib_backgroundjob_element_t));
  if(IS_NULL_PTR(instance)) return NULL;
  // The reference held by progress->gui_data itself, dropped by _destroyed_gui_thread().
  dt_atomic_set_int(&instance->refcount, 1);

  _added_gui_thread_t *params = (_added_gui_thread_t *)malloc(sizeof(_added_gui_thread_t));
  if(IS_NULL_PTR(params))
  {
    dt_free(instance);
    return NULL;
  }
  params->self_widget = self->widget;
  params->instance = _instance_ref(instance);
  params->message = g_strdup(message);
  params->has_progress_bar = has_progress_bar;
  g_main_context_invoke(NULL, _added_gui_thread, params);

  // return the gui thingy container
  return instance;
}

typedef struct _destroyed_gui_thread_t
{
  dt_lib_module_t *self;
  dt_lib_backgroundjob_element_t *instance;
} _destroyed_gui_thread_t;

static gboolean _destroyed_gui_thread(gpointer user_data)
{
  _destroyed_gui_thread_t *params = (_destroyed_gui_thread_t *)user_data;

  // Mark first: if _added_gui_thread() for this same instance hasn't run yet, this tells it to
  // skip building/packing widgets nobody will ever remove (see the struct comment on `destroyed`).
  params->instance->destroyed = TRUE;

  /* remove job widget from jobbox */
  if(params->instance->widget && GTK_IS_WIDGET(params->instance->widget))
    gtk_container_remove(GTK_CONTAINER(params->self->widget), params->instance->widget);

  // gtk_container_remove() above drops the last ref on the whole subtree (widget, hbox, label,
  // progressbar), destroying it. NULL every pointer so any update/message/cancellable callback
  // still in flight for this instance (see the refcount comment on the struct) finds them NULL
  // and skips touching freed GTK widgets instead of crashing.
  params->instance->widget = NULL;
  params->instance->label = NULL;
  params->instance->progressbar = NULL;
  params->instance->hbox = NULL;

  /* if jobbox is empty let's hide */
  if(!dt_gui_container_has_children(GTK_CONTAINER(params->self->widget)))
    gtk_widget_hide(params->self->widget);

  _instance_unref(params->instance);
  dt_free(params);
  return FALSE;
}

// remove the gui that is pointed to in instance
static void _lib_backgroundjobs_destroyed(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance)
{
  if(IS_NULL_PTR(instance)) return;
  _destroyed_gui_thread_t *params = (_destroyed_gui_thread_t *)malloc(sizeof(_destroyed_gui_thread_t));
  if(IS_NULL_PTR(params)) return;
  params->self = self;
  params->instance = instance;
  g_main_context_invoke(NULL, _destroyed_gui_thread, params);
}

static void _lib_backgroundjobs_cancel_callback_new(GtkWidget *w, gpointer user_data)
{
  dt_progress_t *progress = (dt_progress_t *)user_data;
  dt_control_progress_cancel(darktable.control, progress);
}

typedef struct _cancellable_gui_thread_t
{
  dt_lib_backgroundjob_element_t *instance;
  dt_progress_t *progress;
} _cancellable_gui_thread_t;

static gboolean _cancellable_gui_thread(gpointer user_data)
{
  _cancellable_gui_thread_t *params = (_cancellable_gui_thread_t *)user_data;

  if(!IS_NULL_PTR(params->instance->hbox))
  {
    GtkBox *hbox = GTK_BOX(params->instance->hbox);
    GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_cancel, 0, NULL);
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_backgroundjobs_cancel_callback_new), params->progress);
    gtk_box_pack_start(hbox, GTK_WIDGET(button), FALSE, FALSE, 0);
    gtk_widget_show_all(button);
  }

  _instance_unref(params->instance);
  dt_free(params);
  return FALSE;
}

static void _lib_backgroundjobs_cancellable(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                            dt_progress_t *progress)
{
  // add a cancel button to the gui. when clicked we want dt_control_progress_cancel(darktable.control,
  // progress); to be called
  if(!darktable.control->running || IS_NULL_PTR(instance)) return;

  _cancellable_gui_thread_t *params = (_cancellable_gui_thread_t *)malloc(sizeof(_cancellable_gui_thread_t));
  if(IS_NULL_PTR(params)) return;
  params->instance = _instance_ref(instance);
  params->progress = progress;
  g_main_context_invoke(NULL, _cancellable_gui_thread, params);
}

typedef struct _update_gui_thread_t
{
  dt_lib_backgroundjob_element_t *instance;
  double value;
} _update_gui_thread_t;

static gboolean _update_gui_thread(gpointer user_data)
{
  _update_gui_thread_t *params = (_update_gui_thread_t *)user_data;

  if(!IS_NULL_PTR(params->instance->progressbar))
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(params->instance->progressbar), CLAMP(params->value, 0, 1.0));

  _instance_unref(params->instance);
  dt_free(params);
  return FALSE;
}

static void _lib_backgroundjobs_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                        double value)
{
  // update the progress bar
  if(!darktable.control->running || IS_NULL_PTR(instance)) return;

  _update_gui_thread_t *params = (_update_gui_thread_t *)malloc(sizeof(_update_gui_thread_t));
  if(IS_NULL_PTR(params)) return;
  params->instance = _instance_ref(instance);
  params->value = value;
  g_main_context_invoke(NULL, _update_gui_thread, params);
}

typedef struct _update_label_gui_thread_t
{
  dt_lib_backgroundjob_element_t *instance;
  char *message;
} _update_label_gui_thread_t;

static gboolean _update_message_gui_thread(gpointer user_data)
{
  _update_label_gui_thread_t *params = (_update_label_gui_thread_t *)user_data;

  if(!IS_NULL_PTR(params->instance->label))
    gtk_label_set_text(GTK_LABEL(params->instance->label), params->message);

  _instance_unref(params->instance);
  dt_free(params->message);
  dt_free(params);
  return FALSE;
}

static void _lib_backgroundjobs_message_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                                const char *message)
{
  // update the progress bar
  if(!darktable.control->running || IS_NULL_PTR(instance)) return;

  _update_label_gui_thread_t *params = (_update_label_gui_thread_t *)malloc(sizeof(_update_label_gui_thread_t));
  if(IS_NULL_PTR(params)) return;
  params->instance = _instance_ref(instance);
  params->message = g_strdup(message);
  g_main_context_invoke(NULL, _update_message_gui_thread, params);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
