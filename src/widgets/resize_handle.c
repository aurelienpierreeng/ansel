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

#include "widgets/resize_handle.h"
#include "widgets/widget_settings.h"

#include "system/macros.h"

#include <math.h>

typedef struct dtgtk_resize_handle_t
{
  GtkOrientation orientation;
  gboolean invert;
  dtgtk_resize_handle_get_size_f get_size;
  dtgtk_resize_handle_resize_f resize;
  gpointer user_data;
  gboolean dragging;
  double start_root;
  int start_size;
  int current_size;
} dtgtk_resize_handle_t;

static gboolean _resize_handle_cursor(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dtgtk_resize_handle_t *handle = (dtgtk_resize_handle_t *)user_data;
  if(event->type == GDK_ENTER_NOTIFY)
  {
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_PRELIGHT, FALSE);
    dt_widget_set_cursor(handle->orientation == GTK_ORIENTATION_VERTICAL
                             ? GDK_SB_V_DOUBLE_ARROW
                             : GDK_SB_H_DOUBLE_ARROW);
  }
  else if(!handle->dragging)
  {
    gtk_widget_unset_state_flags(widget, GTK_STATE_FLAG_PRELIGHT);
    dt_widget_set_cursor(GDK_LEFT_PTR);
  }

  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _resize_handle_button(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dtgtk_resize_handle_t *handle = (dtgtk_resize_handle_t *)user_data;
  if(event->button != 1) return TRUE;

  if(event->type == GDK_BUTTON_PRESS)
  {
    handle->dragging = TRUE;
    handle->start_root = (handle->orientation == GTK_ORIENTATION_VERTICAL) ? event->y_root : event->x_root;
    handle->start_size = handle->get_size(handle->user_data);
    handle->current_size = handle->start_size;
    gtk_grab_add(widget);
    dt_widget_set_cursor(handle->orientation == GTK_ORIENTATION_VERTICAL
                             ? GDK_SB_V_DOUBLE_ARROW
                             : GDK_SB_H_DOUBLE_ARROW);
  }
  else if(event->type == GDK_BUTTON_RELEASE)
  {
    handle->dragging = FALSE;
    gtk_grab_remove(widget);
    handle->current_size = handle->resize(handle->current_size, TRUE, handle->user_data);

    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const gboolean pointer_on_handle = event->x >= 0. && event->x <= allocation.width
                                      && event->y >= 0. && event->y <= allocation.height;
    if(pointer_on_handle)
      gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_PRELIGHT, FALSE);
    else
      gtk_widget_unset_state_flags(widget, GTK_STATE_FLAG_PRELIGHT);

    dt_widget_set_cursor(pointer_on_handle
                             ? (handle->orientation == GTK_ORIENTATION_VERTICAL
                                ? GDK_SB_V_DOUBLE_ARROW
                                : GDK_SB_H_DOUBLE_ARROW)
                             : GDK_LEFT_PTR);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static gboolean _resize_handle_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dtgtk_resize_handle_t *handle = (dtgtk_resize_handle_t *)user_data;
  if(!handle->dragging) return TRUE;

  /* Each motion event is one sample in the GTK pointer stream. Convert only the
   * axis matching the resize direction and leave clamping/application to the
   * owner callback, which is the code owning the resized widget. */
  const double root = (handle->orientation == GTK_ORIENTATION_VERTICAL) ? event->y_root : event->x_root;
  double delta = root - handle->start_root;
  if(handle->invert) delta = -delta;
  const int requested_size = handle->start_size + (int)round(delta);
  handle->current_size = handle->resize(requested_size, FALSE, handle->user_data);
  return TRUE;
}

GtkWidget *dtgtk_resize_handle_new(GtkOrientation orientation, gboolean invert, const char *tooltip,
                                        dtgtk_resize_handle_get_size_f get_size,
                                        dtgtk_resize_handle_resize_f resize, gpointer user_data)
{
  // A GtkEventBox is enough: it owns a GdkWindow for pointer events and renders its CSS
  // background/border, so the whole hover affordance lives in the stylesheet (.resize-handle)
  // with no custom drawing. It is meant to be added as an overlay child on the resized widget.
  GtkWidget *handle_widget = gtk_event_box_new();
  dtgtk_resize_handle_t *handle = g_malloc0(sizeof(*handle));
  handle->orientation = orientation;
  handle->invert = invert;
  handle->get_size = get_size;
  handle->resize = resize;
  handle->user_data = user_data;

  // Pin the grip to the edge that grows the target when dragged outward, filling the other axis.
  // `invert` means the target grows in the negative direction, so the grip sits on the start edge.
  // The grab thickness is set here (a CSS min-height/width isn't honoured for an empty overlay
  // child -- GTK allocates its ~0px natural size); an edge class is added for hover styling.
  GtkStyleContext *ctx = gtk_widget_get_style_context(handle_widget);
  gtk_style_context_add_class(ctx, "resize-handle");
  if(orientation == GTK_ORIENTATION_VERTICAL)
  {
    gtk_widget_set_size_request(handle_widget, -1, DT_PIXEL_APPLY_DPI(5));
    gtk_widget_set_halign(handle_widget, GTK_ALIGN_FILL);
    gtk_widget_set_valign(handle_widget, invert ? GTK_ALIGN_START : GTK_ALIGN_END);
    gtk_style_context_add_class(ctx, invert ? "resize-handle-top" : "resize-handle-bottom");
  }
  else
  {
    gtk_widget_set_size_request(handle_widget, DT_PIXEL_APPLY_DPI(5), -1);
    gtk_widget_set_valign(handle_widget, GTK_ALIGN_FILL);
    gtk_widget_set_halign(handle_widget, invert ? GTK_ALIGN_START : GTK_ALIGN_END);
    gtk_style_context_add_class(ctx, invert ? "resize-handle-left" : "resize-handle-right");
  }

  gtk_widget_set_events(handle_widget, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                      | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                      | GDK_POINTER_MOTION_MASK);
  if(!IS_NULL_PTR(tooltip))
    gtk_widget_set_tooltip_text(handle_widget, tooltip);

  g_object_set_data_full(G_OBJECT(handle_widget), "dtgtk-resize-handle", handle, g_free);
  g_signal_connect(G_OBJECT(handle_widget), "button-press-event", G_CALLBACK(_resize_handle_button), handle);
  g_signal_connect(G_OBJECT(handle_widget), "button-release-event", G_CALLBACK(_resize_handle_button), handle);
  g_signal_connect(G_OBJECT(handle_widget), "motion-notify-event", G_CALLBACK(_resize_handle_motion), handle);
  g_signal_connect(G_OBJECT(handle_widget), "enter-notify-event", G_CALLBACK(_resize_handle_cursor), handle);
  g_signal_connect(G_OBJECT(handle_widget), "leave-notify-event", G_CALLBACK(_resize_handle_cursor), handle);

  return handle_widget;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
