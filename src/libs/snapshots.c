/*
    This file is part of darktable,
    Copyright (C) 2011 Alexandre Prokoudine.
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2011, 2014, 2016 johannes hanika.
    Copyright (C) 2011-2012, 2014-2017 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2018 Tobias Ellinghaus.
    Copyright (C) 2013-2014, 2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 Wolfgang Goetz.
    Copyright (C) 2015 parafin.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2019, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2020 Heiko Bauke.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Bill Ferguson.
    Copyright (C) 2021 Philippe Weyland.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2026 Guillaume STUTIN.

    
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

#include "common/darktable.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/history.h"
#include "common/iop_order.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/dev_history.h"
#include "develop/dev_snapshot.h"
#include "dtgtk/paint.h"

#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <math.h>

DT_MODULE(1)

#define DT_LIB_SNAPSHOTS_COUNT 4
#define SNAP_LOG(...) dt_print(DT_DEBUG_DEV, __VA_ARGS__)
#define HANDLE_SIZE DT_PIXEL_APPLY_DPI_DPP(36)

/* a snapshot */
typedef struct dt_lib_snapshot_t
{
  GtkWidget *row;           // container for button + delete_button; shown/hidden as a unit
  GtkWidget *button;
  GtkWidget *delete_button;
  dt_dev_snapshot_t snap;   // ROI-scoped render, recomputed on pan/zoom, see develop/dev_snapshot.h
  int32_t imgid;
  int32_t history_end;
} dt_lib_snapshot_t;


typedef struct dt_lib_snapshots_t
{
  GtkWidget *snapshots_box;

  uint32_t selected;

  /* current active snapshots */
  uint32_t num_snapshots;

  /* size of snapshots */
  uint32_t size;

  /* snapshots */
  dt_lib_snapshot_t *snapshot;


  /* change snapshot overlay controls */
  gboolean dragging, vertical, inverted;
  double vp_x, vp_y, vp_width, vp_height, vp_xpointer, vp_ypointer, vp_xrotate, vp_yrotate;
  gboolean on_going;
  gboolean hover_rotation;

  GtkWidget *take_button;
} dt_lib_snapshots_t;

/* callback for take snapshot */
static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, gpointer user_data);
static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, gpointer user_data);
static void _lib_snapshots_delete_button_clicked_callback(GtkWidget *widget, gpointer user_data);

// Reset the value fields to "empty" without releasing the snapshot engine or touching GTK
// widgets. Used when a snapshot's engine is being handed off to another slot (compacting the
// list after a delete) rather than dropped -- the caller is responsible for the engine.
// dt_dev_snapshot_t is just a refcounted-engine pointer (develop/dev_snapshot.h), so the move
// itself already happened via the plain struct assignment at the call site; this only needs to
// forget this now-empty slot's copy of that pointer, never release it.
static void _lib_snapshot_reset_fields(dt_lib_snapshot_t *snap)
{
  snap->snap.engine = NULL;
  snap->imgid = UNKNOWN_IMAGE;
  snap->history_end = 0;
}

static void _lib_snapshot_clear_state(dt_lib_snapshot_t *snap)
{
  if(IS_NULL_PTR(snap)) return;
  dt_dev_snapshot_clear(&snap->snap);
  snap->imgid = UNKNOWN_IMAGE;
  snap->history_end = 0;
}

// Freeze the current darkroom develop state and render it at `source`'s current viewport (ROI),
// recomputed as pan/zoom change afterward -- see develop/dev_snapshot.h. The frozen context and
// its pipe are kept alive for the snapshot's whole lifetime, released by _lib_snapshot_clear_state().
// Returns 0 on success, 1 on failure.
static int _lib_snapshot_capture_state(dt_lib_snapshot_t *snapshot, dt_develop_t *source)
{
  if(IS_NULL_PTR(snapshot) || IS_NULL_PTR(source))
  {
    SNAP_LOG("[snapshots] capture failed: invalid inputs snapshot=%p source=%p\n", (void *)snapshot,
             (void *)source);
    return 1;
  }
  if(source->image_storage.id <= 0)
  {
    SNAP_LOG("[snapshots] capture failed: invalid source imgid=%d\n", source->image_storage.id);
    return 1;
  }

  _lib_snapshot_clear_state(snapshot);

  // dt_dev_snapshot_capture() takes ownership of these two lists -- duplicate the live,
  // possibly-uncommitted history/iop_order under the lock here, since it must not read
  // source->history_mutex-guarded state itself (it also serves duplicate.c, which captures a
  // *different*, not-currently-open image and has no such live source to lock).
  GList *history_copy = NULL;
  GList *iop_order_copy = NULL;
  int32_t history_end = 0;

  dt_pthread_rwlock_rdlock(&source->history_mutex);
  history_copy = dt_history_duplicate(source->history);
  iop_order_copy = dt_ioppr_iop_order_copy_deep(source->iop_order_list);
  history_end = dt_dev_get_history_end_ext(source);
  dt_pthread_rwlock_unlock(&source->history_mutex);

  snapshot->imgid = source->image_storage.id;
  snapshot->history_end = history_end;

  dt_control_change_cursor_by_name_and_flush("progress");
  const gboolean ok = dt_dev_snapshot_capture(&snapshot->snap, source, source->image_storage.id,
                                              history_copy, iop_order_copy, history_end);
  dt_control_commit_cursor();

  SNAP_LOG("[snapshots] capture: imgid=%d history_end=%d image=%s\n",
           snapshot->imgid, snapshot->history_end, ok ? "ok" : "FAILED");

  return ok ? 0 : 1;
}

const char *name(struct dt_lib_module_t *self)
{
  return _("Snapshots");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 800;
}

// draw snapshot sign
static void _draw_sym(cairo_t *cr, float x, float y, gboolean vertical, gboolean inverted)
{
  const double inv = inverted ? -0.1 : 1.0;

  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(12) * PANGO_SCALE);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, C_("snapshot sign", "S"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);

  if(vertical)
    cairo_move_to(cr, x - (inv * ink.width * 1.2f), y - (ink.height / 2.0f) - DT_PIXEL_APPLY_DPI(3));
  else
    cairo_move_to(cr, x - (ink.width / 2.0), y + (-inv * (ink.height * 1.2f) - DT_PIXEL_APPLY_DPI(2)));

  dt_draw_set_color_overlay(cr, FALSE, 0.9);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

/* expose snapshot over center viewport */
void gui_post_expose(dt_lib_module_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(IS_NULL_PTR(d)) return;
  dt_develop_t *dev = darktable.develop;

  if(d->selected >= 1 && d->selected <= d->size)
  {
    dt_lib_snapshot_t *snap = d->snapshot + (d->selected - 1);
    if(dt_dev_snapshot_is_valid(&snap->snap))
    {
      float image_box[4] = { 0.0f };
      dt_dev_get_image_box_in_widget(dev, width, height, image_box);
      if(image_box[2] <= 0.0f || image_box[3] <= 0.0f) return;
      d->vp_x = image_box[0];
      d->vp_y = image_box[1];
      d->vp_width = image_box[2];
      d->vp_height = image_box[3];

      const double split_x = CLAMP(d->vp_xpointer, 0.0, 1.0);
      const double split_y = CLAMP(d->vp_ypointer, 0.0, 1.0);

      /* set x,y,w,h of surface depending on split align and invert */
      double x = d->vp_x;
      double y = d->vp_y;
      double w = d->vp_width;
      double h = d->vp_height;
      if(d->vertical)
      {
        x = d->inverted ? d->vp_x + d->vp_width * split_x : d->vp_x;
        w = d->inverted ? d->vp_width * (1.0 - split_x) : d->vp_width * split_x;
      }
      else
      {
        y = d->inverted ? d->vp_y + d->vp_height * split_y : d->vp_y;
        h = d->inverted ? d->vp_height * (1.0 - split_y) : d->vp_height * split_y;
      }

      const double size = DT_PIXEL_APPLY_DPI(d->inverted ? -15 : 15);

      dt_dev_snapshot_draw(&snap->snap, cri, dev, width, height, x, y, w, h);

      // draw the split line using the selected overlay color
      dt_draw_set_color_overlay(cri, TRUE, 0.7);
      cairo_set_line_width(cri, 1.);

      if(d->vertical)
      {
        const double lx = d->vp_x + d->vp_width * split_x;
        const double center = d->vp_y + 0.5 * d->vp_height;

        cairo_move_to(cri, lx, d->vp_y);
        cairo_line_to(cri, lx, d->vp_y + d->vp_height);
        cairo_stroke(cri);

        if(!d->dragging)
        {
          cairo_move_to(cri, lx, center - size);
          cairo_line_to(cri, lx - (size * 1.2), center);
          cairo_line_to(cri, lx, center + size);
          cairo_close_path(cri);
          cairo_fill(cri);
          _draw_sym(cri, lx, center, TRUE, d->inverted);
        }
      }
      else
      {
        const double ly = d->vp_y + d->vp_height * split_y;
        const double center = d->vp_x + 0.5 * d->vp_width;

        cairo_move_to(cri, d->vp_x, ly);
        cairo_line_to(cri, d->vp_x + d->vp_width, ly);
        cairo_stroke(cri);

        if(!d->dragging)
        {
          cairo_move_to(cri, center - size, ly);
          cairo_line_to(cri, center, ly - (size * 1.2));
          cairo_line_to(cri, center + size, ly);
          cairo_close_path(cri);
          cairo_fill(cri);
          _draw_sym(cri, center, ly, FALSE, d->inverted);
        }
      }

      /* if mouse over control, draw center rotate handle (hidden while dragging) */
      if(!d->dragging)
      {
        const double half_handle_size = HANDLE_SIZE * 0.5;
        const gint rx = (d->vertical ? d->vp_x + d->vp_width * split_x : d->vp_x + d->vp_width * 0.5)
                        - half_handle_size;
        const gint ry = (d->vertical ? d->vp_y + d->vp_height * 0.5 : d->vp_y + d->vp_height * split_y)
                        - half_handle_size;

        dt_draw_set_color_overlay(cri, TRUE, d->hover_rotation ? 1.0 : 0.3);
        cairo_set_line_width(cri, 0.5);
        dtgtk_cairo_paint_refresh(cri, rx, ry, HANDLE_SIZE, HANDLE_SIZE, 0, NULL);
      }

      d->on_going = FALSE;

      if(d->hover_rotation) dt_control_queue_cursor_by_name("exchange");
      else if(d->dragging) dt_control_queue_cursor_by_name("grabbing");
      else
      {
        dt_view_t *view = darktable.view_manager->proxy.darkroom.view;
        if(!IS_NULL_PTR(view) && !IS_NULL_PTR(darktable.view_manager->proxy.darkroom.set_default_cursor))
          darktable.view_manager->proxy.darkroom.set_default_cursor(view, pointerx, pointery);
        else
          dt_control_queue_cursor_by_name("left_ptr");
      }
    }
  }
}

int button_released(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  const gboolean visible_picker = dt_iop_color_picker_is_visible(darktable.develop);

  if(!visible_picker && d->selected > 0 && which == 1)
  {
    if(d->dragging)
    {
      d->dragging = FALSE;
      d->hover_rotation = FALSE;
    }
    // Refresh mouse_moved event
    return mouse_moved(self, x, y, 0.0, which);
  }
  return 0;
}

static int _lib_snapshot_rotation_cnt = 0;

int button_pressed(struct dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  // only react to left click
  if(which != 1) return 0;

  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  const gboolean visible_picker = dt_iop_color_picker_is_visible(darktable.develop);
  
  if(!visible_picker && d->selected > 0)
  {
    if(d->on_going) return 1;
    if(d->vp_width <= 0.0 || d->vp_height <= 0.0) return 0;
    if(x < d->vp_x || x > d->vp_x + d->vp_width || y < d->vp_y || y > d->vp_y + d->vp_height) return 0;

    const double xp = CLAMP((x - d->vp_x) / d->vp_width, 0.0, 1.0);
    const double yp = CLAMP((y - d->vp_y) / d->vp_height, 0.0, 1.0);

    if(d->hover_rotation)
    {
      /* let's rotate */
      _lib_snapshot_rotation_cnt++;

      d->vertical = !d->vertical;
      if(_lib_snapshot_rotation_cnt % 2) d->inverted = !d->inverted;

      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
      d->vp_xrotate = xp;
      d->vp_yrotate = yp;
      d->on_going = TRUE;
      dt_control_queue_redraw_center();
    }
    /* do the dragging !? -- only grab the split line if the click lands within the mouse
       action radius of its current position, same hit-test radius (DT_GUI_MOUSE_EFFECT_RADIUS)
       used to grab a mask node, rather than anywhere in the image */
    else
    {
      const double split_x = CLAMP(d->vp_xpointer, 0.0, 1.0);
      const double split_y = CLAMP(d->vp_ypointer, 0.0, 1.0);
      const double lx = d->vp_x + d->vp_width * split_x;
      const double ly = d->vp_y + d->vp_height * split_y;
      const double dist_to_line = d->vertical ? fabs(x - lx) : fabs(y - ly);
      if(dist_to_line > DT_GUI_MOUSE_EFFECT_RADIUS) return 0;

      d->dragging = TRUE;
      d->vp_ypointer = yp;
      d->vp_xpointer = xp;
      d->vp_xrotate = 0.0;
      d->vp_yrotate = 0.0;
      dt_control_queue_redraw_center();
    }
    return 1;
  }
  return 0;
}

int mouse_moved(dt_lib_module_t *self, double x, double y, double pressure, int which)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  const gboolean visible_picker = dt_iop_color_picker_is_visible(darktable.develop);

  if(!visible_picker && d->selected > 0)
  {
    if(d->vp_width <= 0.0 || d->vp_height <= 0.0) return 0;
    const double xp = CLAMP((x - d->vp_x) / d->vp_width, 0.0, 1.0);
    const double yp = CLAMP((y - d->vp_y) / d->vp_height, 0.0, 1.0);

    if(d->dragging)
    {
      /* update pointer pos */
      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
      dt_control_queue_redraw_center();
      return 1;
    }

    // Not dragging: only claim the move event while hovering the rotate handle (it needs the
    // "exchange" cursor feedback in gui_post_expose) -- otherwise release it, so normal pan/hover
    // elsewhere in the darkroom isn't silently blocked for as long as a snapshot stays selected.
    // dt_view_manager_mouse_moved() (views/view.c) only forwards the event to the darkroom view's
    // own handler when no plugin claims it, so an unconditional `return 1` here would starve it
    // permanently while any snapshot is toggled on.
    const gboolean was_hovering = d->hover_rotation;
    const double split_x = CLAMP(d->vp_xpointer, 0.0, 1.0);
    const double split_y = CLAMP(d->vp_ypointer, 0.0, 1.0);
    const double handle_mouse = (DT_GUI_MOUSE_EFFECT_RADIUS + HANDLE_SIZE) * 0.5;
    const double rxc = d->vertical ? d->vp_x + d->vp_width * split_x : d->vp_x + d->vp_width * 0.5;
    const double ryc = d->vertical ? d->vp_y + d->vp_height * 0.5 : d->vp_y + d->vp_height * split_y;
    const double dx = x - rxc;
    const double dy = y - ryc;
    d->hover_rotation = (dx * dx + dy * dy) < (handle_mouse * handle_mouse);

    if(!d->hover_rotation)
    {
      if(was_hovering) dt_control_queue_redraw_center(); // clear the stale handle highlight
      return 0;
    }

    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  d->num_snapshots = 0;
  d->selected = 0;
  d->hover_rotation = FALSE;

  for(uint32_t k = 0; k < d->size; k++)
  {
    _lib_snapshot_clear_state(d->snapshot + k);
    gtk_widget_hide(d->snapshot[k].row);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);
  }

  dt_control_queue_redraw_center();
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)g_malloc0(sizeof(dt_lib_snapshots_t));
  self->data = (void *)d;

  /* initialize snapshot storages */
  d->size = 4;
  d->snapshot = (dt_lib_snapshot_t *)g_malloc0_n(d->size, sizeof(dt_lib_snapshot_t));
  d->vp_x = 0.0;
  d->vp_y = 0.0;
  d->vp_width = 1.0;
  d->vp_height = 1.0;
  d->vp_xpointer = 0.5;
  d->vp_ypointer = 0.5;
  d->vp_xrotate = 0.0;
  d->vp_yrotate = 0.0;
  d->vertical = TRUE;
  d->on_going = FALSE;
  d->hover_rotation = FALSE;
  /* initialize ui containers */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  d->snapshots_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  /* create take snapshot button */
  d->take_button = dt_action_button_new(self, N_("take snapshot"), _lib_snapshots_add_button_clicked_callback, self,
                                        _("take snapshot to compare with another image "
                                          "or the same image at another stage of development"), 0, 0);

  for(int k = 0; k < d->size; k++)
  {
    d->snapshot[k].button = gtk_toggle_button_new_with_label("");
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(d->snapshot[k].button));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(d->snapshot[k].button, TRUE);

    g_signal_connect(G_OBJECT(d->snapshot[k].button), "clicked",
                     G_CALLBACK(_lib_snapshots_toggled_callback), self);

    g_object_set_data(G_OBJECT(d->snapshot[k].button), "snapshot", GINT_TO_POINTER(k + 1));

    // Same trash icon as the shapes list under "drawn mask" in develop/blend_gui.c
    // (group_delete_col): themed "user-trash-symbolic", not a dtgtk cairo glyph.
    d->snapshot[k].delete_button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(d->snapshot[k].delete_button), GTK_RELIEF_NONE);
    gtk_button_set_image(GTK_BUTTON(d->snapshot[k].delete_button),
                         gtk_image_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(d->snapshot[k].delete_button, _("remove this snapshot"));
    g_object_set_data(G_OBJECT(d->snapshot[k].delete_button), "snapshot", GINT_TO_POINTER(k + 1));
    g_signal_connect(G_OBJECT(d->snapshot[k].delete_button), "clicked",
                     G_CALLBACK(_lib_snapshots_delete_button_clicked_callback), self);

    d->snapshot[k].row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(d->snapshot[k].row), d->snapshot[k].button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(d->snapshot[k].row), d->snapshot[k].delete_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(d->snapshots_box), d->snapshot[k].row, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(d->snapshot[k].row, TRUE);
  }

  /* add snapshot box and take snapshot button to widget ui*/
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->snapshots_box, 1, "plugins/darkroom/snapshots/windowheight",
                                       DT_UI_RESIZE_DYNAMIC), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->take_button), TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  for(uint32_t k = 0; k < d->size; k++) _lib_snapshot_clear_state(d->snapshot + k);
  dt_free(d->snapshot);

  dt_free(self->data);
}

static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(IS_NULL_PTR(d)) return;
  if(d->size <= 0) return;

  // Capture into a scratch slot first, off to the side of the visible array -- only once it has
  // actually succeeded do we touch the slot rotation and GTK labels below. This guarantees a
  // failed capture (e.g. out of memory) leaves the panel in exactly the state it was in before
  // this click, instead of a rotated slot 0 stuck showing a label for a snapshot that was never
  // created.
  dt_lib_snapshot_t scratch = { 0 };
  if(_lib_snapshot_capture_state(&scratch, darktable.develop))
  {
    _lib_snapshot_clear_state(&scratch);
    return;
  }

  /* backup last snapshot slot */
  dt_lib_snapshot_t last = d->snapshot[d->size - 1];

  /* rotate slots down to make room for new one on top */
  for(int k = d->size - 1; k > 0; k--)
  {
    GtkWidget *r = d->snapshot[k].row;
    GtkWidget *b = d->snapshot[k].button;
    GtkWidget *db = d->snapshot[k].delete_button;
    d->snapshot[k] = d->snapshot[k - 1];
    d->snapshot[k].row = r;
    d->snapshot[k].button = b;
    d->snapshot[k].delete_button = db;
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k].button))),
      gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k - 1].button)))));
  }

  /* update top slot with the already-captured scratch snapshot */
  GtkWidget *r = d->snapshot[0].row;
  GtkWidget *b = d->snapshot[0].button;
  GtkWidget *db = d->snapshot[0].delete_button;
  d->snapshot[0] = last;
  d->snapshot[0].row = r;
  d->snapshot[0].button = b;
  d->snapshot[0].delete_button = db;
  // `last` may itself have held a valid engine (all `d->size` slots were already full) --
  // release it before overwriting with the scratch capture, or it would leak (nothing else
  // references it: `last` was never shown/toggled, so no other slot's `.snap` points to it).
  dt_dev_snapshot_clear(&d->snapshot[0].snap);
  d->snapshot[0].snap = scratch.snap;
  d->snapshot[0].imgid = scratch.imgid;
  d->snapshot[0].history_end = scratch.history_end;

  char label[64];
  const gchar *name = _("original");
  gchar *dynamic_name = NULL;
  if(dt_dev_get_history_end_ext(darktable.develop) > 0)
  {
    dt_dev_history_item_t *history_item = g_list_nth_data(darktable.develop->history,
                                                          dt_dev_get_history_end_ext(darktable.develop) - 1);
    if(!IS_NULL_PTR(history_item) && !IS_NULL_PTR(history_item->module))
    {
      dynamic_name = dt_history_item_get_name(history_item->module);
      if(!IS_NULL_PTR(dynamic_name)) name = dynamic_name;
    }
    else
      name = _("unknown");
  }
  g_snprintf(label, sizeof(label), "%s (%d)", name, dt_dev_get_history_end_ext(darktable.develop));
  if(!IS_NULL_PTR(dynamic_name)) dt_free(dynamic_name);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[0].button))), label);

  /* update slots used */
  if(d->num_snapshots != d->size) d->num_snapshots++;

  /* show active snapshot slots. row has no-show-all set (so ambient show_all() calls on
   * an ancestor leave inactive slots hidden), which means show_all() on row itself is
   * *also* a no-op -- it must be shown explicitly, and so must its children since they
   * were never individually shown either. */
  for(uint32_t k = 0; k < d->num_snapshots; k++)
  {
    gtk_widget_show(d->snapshot[k].row);
    gtk_widget_show(d->snapshot[k].button);
    gtk_widget_show(d->snapshot[k].delete_button);
  }
}

static void _lib_snapshots_delete_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(IS_NULL_PTR(d)) return;

  const uint32_t which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "snapshot"));
  if(which < 1 || which > d->num_snapshots) return;
  const uint32_t p = which - 1;

  if(d->selected == which) d->selected = 0;
  else if(d->selected > which) d->selected--;

  // Free the deleted snapshot's surfaces now, before its slot gets overwritten by the shift
  // below -- otherwise they would be silently leaked (overwritten with no destroy call).
  _lib_snapshot_clear_state(d->snapshot + p);

  /* shift every older slot up by one to fill the gap, keeping row/button/delete_button
   * pinned to their screen position (same pattern as the "take snapshot" rotation). Each
   * `d->snapshot[k] = d->snapshot[k + 1]` *moves* the slot's engine pointer, so nothing here
   * may release it: the slot being overwritten already handed its own reference to k-1 in the
   * previous iteration (or, for k == p, it was just cleared above). */
  for(uint32_t k = p; k < d->num_snapshots - 1; k++)
  {
    GtkWidget *r = d->snapshot[k].row;
    GtkWidget *b = d->snapshot[k].button;
    GtkWidget *db = d->snapshot[k].delete_button;
    d->snapshot[k] = d->snapshot[k + 1];
    d->snapshot[k].row = r;
    d->snapshot[k].button = b;
    d->snapshot[k].delete_button = db;
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k].button))),
      gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->snapshot[k + 1].button)))));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), (k + 1) == d->selected);
  }

  // The last active slot's surfaces (if any) were just relocated into the slot above it by
  // the loop's last iteration, so only reset its fields here -- destroying them would be a
  // double-free of surfaces now owned by that other slot.
  const uint32_t last = d->num_snapshots - 1;
  _lib_snapshot_reset_fields(d->snapshot + last);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[last].button), FALSE);
  gtk_widget_hide(d->snapshot[last].row);
  d->num_snapshots--;

  dt_control_queue_redraw_center();
}

static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  /* get current snapshot index */
  int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "snapshot"));

  /* check if snapshot is activated */
  if(gtk_toggle_button_get_active(widget))
  {
    /* lets deactivate all togglebuttons except for self */
    for(uint32_t k = 0; k < d->size; k++)
      if(GTK_WIDGET(widget) != d->snapshot[k].button)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);

    const dt_lib_snapshot_t *s = d->snapshot + (which - 1);
    d->selected = dt_dev_snapshot_is_valid(&s->snap) ? which : 0;
  }
  else if(d->selected == (uint32_t)which)
  {
    d->selected = 0;
  }

  /* redraw center view */
  dt_control_queue_redraw_center();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
