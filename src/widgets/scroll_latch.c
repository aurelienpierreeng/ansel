/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Guillaume Stutin.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "widgets/scroll_latch.h"
#include "system/macros.h"            // IS_NULL_PTR
#include "system/mem_alloc.h"         // dt_free

// How long the wheel may rest before a step goes to the widget under the pointer again.
#define DT_SCROLL_LATCH_REST_MS 500
// How far the pointer may drift from where the last step found it: a hand on the wheel nudges the mouse.
#define DT_SCROLL_LATCH_DRIFT_PX 4.0

#define DT_SCROLL_LATCH_KEY "dt-scroll-latch"

typedef struct dt_scroll_latch_t
{
  GtkWidget *scrolled_window;
  GtkEventController *steps;  // on the child, capture phase while latched, none otherwise
  GtkEventController *motion; // on the child, capture phase while latched, none otherwise
  gdouble x_root;             // pointer at the last step the window took
  gdouble y_root;
  guint rest;                 // timeout ending the latch, 0 while not latched
} dt_scroll_latch_t;

static void _latch_release(dt_scroll_latch_t *latch)
{
  gtk_event_controller_set_propagation_phase(latch->steps, GTK_PHASE_NONE);
  gtk_event_controller_set_propagation_phase(latch->motion, GTK_PHASE_NONE);
  if(latch->rest) g_source_remove(latch->rest);
  latch->rest = 0;
}

static gboolean _latch_rested(gpointer data)
{
  dt_scroll_latch_t *latch = (dt_scroll_latch_t *)data;
  latch->rest = 0;
  _latch_release(latch);
  return G_SOURCE_REMOVE;
}

// Runs before the window scrolls, for a step no child took and for a step the latch hands over.
static gboolean _window_step(GtkWidget *widget, const GdkEventScroll *event, dt_scroll_latch_t *latch)
{
  // The window takes a step only along an axis that shows a scrollbar.
  GtkScrolledWindow *window = GTK_SCROLLED_WINDOW(widget);
  if(!gtk_widget_get_child_visible(gtk_scrolled_window_get_vscrollbar(window))
     && !gtk_widget_get_child_visible(gtk_scrolled_window_get_hscrollbar(window)))
    return FALSE;

  latch->x_root = event->x_root;
  latch->y_root = event->y_root;
  gtk_event_controller_set_propagation_phase(latch->steps, GTK_PHASE_CAPTURE);
  gtk_event_controller_set_propagation_phase(latch->motion, GTK_PHASE_CAPTURE);
  if(latch->rest) g_source_remove(latch->rest);
  latch->rest = g_timeout_add(DT_SCROLL_LATCH_REST_MS, _latch_rested, latch);
  return FALSE;
}

// A window still gliding after a touchpad flick is still scrolling.
static void _window_moved(GtkAdjustment *adjustment __attribute__((unused)), gpointer scrolled_window)
{
  dt_scroll_latch_t *latch = g_object_get_data(G_OBJECT(scrolled_window), DT_SCROLL_LATCH_KEY);
  if(latch->rest == 0) return;
  g_source_remove(latch->rest);
  latch->rest = g_timeout_add(DT_SCROLL_LATCH_REST_MS, _latch_rested, latch);
}

// The steps controller has taken the step from the widget under the pointer: the window gets it instead.
static void _step_to_window(dt_scroll_latch_t *latch)
{
  GdkEvent *event = gtk_get_current_event();
  if(IS_NULL_PTR(event)) return;
  gtk_widget_event(latch->scrolled_window, event);
  gdk_event_free(event);
}

static void _step(GtkEventControllerScroll *controller __attribute__((unused)), gdouble dx __attribute__((unused)),
                  gdouble dy __attribute__((unused)), dt_scroll_latch_t *latch)
{
  _step_to_window(latch);
}

// The last event of a touchpad gesture carries no delta: it starts the window's deceleration.
static void _step_end(GtkEventControllerScroll *controller __attribute__((unused)), dt_scroll_latch_t *latch)
{
  _step_to_window(latch);
}

// A pointer that moves is aiming at something: the next step goes to what it points at.
static void _pointer_moved(GtkEventControllerMotion *controller __attribute__((unused)), gdouble x __attribute__((unused)),
                           gdouble y __attribute__((unused)), dt_scroll_latch_t *latch)
{
  GdkEvent *event = gtk_get_current_event();
  if(IS_NULL_PTR(event)) return;
  gdouble x_root = 0.0;
  gdouble y_root = 0.0;
  gdk_event_get_root_coords(event, &x_root, &y_root);
  gdk_event_free(event);

  const gdouble dx = x_root - latch->x_root;
  const gdouble dy = y_root - latch->y_root;
  if(dx * dx + dy * dy > DT_SCROLL_LATCH_DRIFT_PX * DT_SCROLL_LATCH_DRIFT_PX) _latch_release(latch);
}

static void _latch_free(gpointer data)
{
  dt_scroll_latch_t *latch = (dt_scroll_latch_t *)data;
  if(latch->rest) g_source_remove(latch->rest);
  g_object_unref(latch->steps);
  g_object_unref(latch->motion);
  dt_free(latch);
}

void dt_ui_scroll_latch(GtkWidget *scrolled_window)
{
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(scrolled_window));
  dt_scroll_latch_t *latch = calloc(1, sizeof(dt_scroll_latch_t));
  latch->scrolled_window = scrolled_window;
  g_object_set_data_full(G_OBJECT(scrolled_window), DT_SCROLL_LATCH_KEY, latch, _latch_free);

  // Held on the child, not on the window: in the capture phase, the window's own handler runs
  // first and stops any deceleration. Held on the window, a touchpad step handed over would
  // start the deceleration that the window's handler then stops.
  latch->steps = gtk_event_controller_scroll_new(child, GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  gtk_event_controller_set_propagation_phase(latch->steps, GTK_PHASE_NONE);
  g_signal_connect(G_OBJECT(latch->steps), "scroll", G_CALLBACK(_step), latch);
  g_signal_connect(G_OBJECT(latch->steps), "scroll-end", G_CALLBACK(_step_end), latch);

  latch->motion = gtk_event_controller_motion_new(child);
  gtk_event_controller_set_propagation_phase(latch->motion, GTK_PHASE_NONE);
  gtk_widget_add_events(child, GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(latch->motion), "motion", G_CALLBACK(_pointer_moved), latch);

  g_signal_connect(G_OBJECT(scrolled_window), "scroll-event", G_CALLBACK(_window_step), latch);
  g_signal_connect_object(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window)), "value-changed",
                          G_CALLBACK(_window_moved), scrolled_window, 0);
  g_signal_connect_object(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scrolled_window)), "value-changed",
                          G_CALLBACK(_window_moved), scrolled_window, 0);
}
