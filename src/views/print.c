/*
    This file is part of darktable,
    Copyright (C) 2014-2015, 2019-2021 Pascal Obry.
    Copyright (C) 2015 Jérémy Rosen.
    Copyright (C) 2015-2016 Tobias Ellinghaus.
    Copyright (C) 2016, 2020 Roman Lebedev.
    Copyright (C) 2017 Dan Torop.
    Copyright (C) 2019-2021 Aldric Renaudin.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Ricky Moon.
    
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

/** this is the view for the print module.  */
#include "common/collection.h"
#include "common/cups_print.h"
#include "common/printing.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/thumbtable.h"

#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "views/view.h"
#include "views/view_api.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE(1)

typedef struct dt_print_t
{
  dt_print_info_t *pinfo;
  dt_images_box *imgs;
  dt_images_box fallback_imgs;
  GList *incoming_selection;
  cairo_surface_t *screen_surfaces[MAX_IMAGE_PER_PAGE];
  dt_view_image_surface_fetcher_t screen_fetchers[MAX_IMAGE_PER_PAGE];
  int32_t last_selected;
  int32_t pending_imgid;
  gboolean busy;
}
dt_print_t;

const char *name(const dt_view_t *self)
{
  return C_("view", "Print");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_PRINT;
}

static void _film_strip_activated(const int32_t imgid, void *data)
{
  const dt_view_t *self = (dt_view_t *)data;
  dt_print_t *prt = (dt_print_t *)self->data;

  prt->last_selected = imgid;
  dt_selection_select_single(darktable.selection, imgid);
  dt_control_set_mouse_over_id(imgid);
  dt_control_set_keyboard_over_id(imgid);
  g_idle_add((GSourceFunc)dt_thumbtable_scroll_to_selection, darktable.gui->ui->thumbtable_filmstrip);
  dt_control_queue_redraw_center();
}

static void _view_print_filmstrip_activate_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  if(imgid > 0) _film_strip_activated(imgid, user_data);
}

static void _view_print_filmstrip_drag_begin_callback(gpointer instance, int32_t imgid, gpointer user_data)
{
  if(imgid <= 0) return;
  dt_selection_select_single(darktable.selection, imgid);
  dt_control_set_mouse_over_id(imgid);
  dt_control_set_keyboard_over_id(imgid);
}

static void _view_print_settings(const dt_view_t *view, dt_print_info_t *pinfo, dt_images_box *imgs)
{
  dt_print_t *prt = (dt_print_t *)view->data;

  prt->pinfo = pinfo;
  prt->imgs = imgs ? imgs : &prt->fallback_imgs;
  if(prt->pending_imgid > UNKNOWN_IMAGE && prt->imgs->imgid_to_load <= UNKNOWN_IMAGE)
    prt->imgs->imgid_to_load = prt->pending_imgid;
  if(prt->imgs->imgid_to_load > UNKNOWN_IMAGE)
    prt->pending_imgid = prt->imgs->imgid_to_load;
  dt_control_queue_redraw();
}

static void _drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                   GtkSelectionData *selection_data, guint target_type, guint time,
                                   gpointer data)
{
  const dt_view_t *self = (dt_view_t *)data;
  dt_print_t *prt = (dt_print_t *)self->data;

  const int bidx = dt_printing_get_image_box(prt->imgs, x, y);

  if(bidx != -1)
    dt_printing_setup_image(prt->imgs, bidx, prt->last_selected,
                            100, 100, ALIGNMENT_CENTER);

  prt->imgs->motion_over = -1;
  dt_control_queue_redraw_center();
}

static gboolean _drag_motion_received(GtkWidget *widget, GdkDragContext *dc,
                                      gint x, gint y, guint time,
                                      gpointer data)
{
  const dt_view_t *self = (dt_view_t *)data;
  dt_print_t *prt = (dt_print_t *)self->data;

  const int bidx = dt_printing_get_image_box(prt->imgs, x, y);
  prt->imgs->motion_over = bidx;

  if(bidx != -1) dt_control_queue_redraw_center();

  return TRUE;
}

void
init(dt_view_t *self)
{
  dt_print_t *prt = calloc(1, sizeof(dt_print_t));
  self->data = prt;
  prt->imgs = &prt->fallback_imgs;
  prt->last_selected = -1;
  prt->pending_imgid = -1;
  prt->incoming_selection = NULL;
  dt_printing_clear_boxes(&prt->fallback_imgs);
  for(int k = 0; k < MAX_IMAGE_PER_PAGE; k++)
    dt_view_image_surface_fetcher_init(&prt->screen_fetchers[k]);

  /* initialize CB to get the print settings from corresponding lib module */
  darktable.view_manager->proxy.print.view = self;
  darktable.view_manager->proxy.print.print_settings = _view_print_settings;
}

void cleanup(dt_view_t *self)
{
  dt_print_t *prt = (dt_print_t *)self->data;
  if(prt->busy) dt_control_log_busy_leave();
  g_list_free(prt->incoming_selection);
  for(int k = 0; k < MAX_IMAGE_PER_PAGE; k++)
    dt_view_image_surface_fetcher_cleanup(&prt->screen_fetchers[k]);
  dt_free(prt);
}

static void expose_print_page(dt_view_t *self, cairo_t *cr,
                              int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_print_t *prt = (dt_print_t *)self->data;

  if(IS_NULL_PTR(prt->pinfo) || prt->pinfo->printer.resolution <= 0 || prt->pinfo->paper.width <= 0.0f
     || prt->pinfo->paper.height <= 0.0f || width <= 1 || height <= 1)
    return;

  float px=.0f, py=.0f, pwidth=.0f, pheight=.0f;
  float ax=.0f, ay=.0f, awidth=.0f, aheight=.0f;

  gboolean borderless = FALSE;

  dt_get_print_layout(prt->pinfo, width, height,
                      &px, &py, &pwidth, &pheight,
                      &ax, &ay, &awidth, &aheight, &borderless);

  // page w/h
  float pg_width  = prt->pinfo->paper.width;
  float pg_height = prt->pinfo->paper.height;

  // non-printable
  float np_top = prt->pinfo->printer.hw_margin_top;
  float np_left = prt->pinfo->printer.hw_margin_left;
  float np_right = prt->pinfo->printer.hw_margin_right;
  float np_bottom = prt->pinfo->printer.hw_margin_bottom;

  // handle the landscape mode if needed
  if(prt->pinfo->page.landscape)
  {
    float tmp = pg_width;
    pg_width = pg_height;
    pg_height = tmp;

    // rotate the non-printable margins
    tmp       = np_top;
    np_top    = np_right;
    np_right  = np_bottom;
    np_bottom = np_left;
    np_left   = tmp;
  }

  const float pright = px + pwidth;
  const float pbottom = py + pheight;

  // x page -> x display
  // (x / pg_width) * p_width + p_x
  cairo_set_source_rgb (cr, 0.9, 0.9, 0.9);
  cairo_rectangle (cr, px, py, pwidth, pheight);
  cairo_fill (cr);

  // record the screen page dimension. this will be used to compute the actual
  // layout of the areas placed over the page.

  dt_printing_setup_display(prt->imgs,
                            px, py, pwidth, pheight,
                            ax, ay, awidth, aheight,
                            borderless);

  // display non-printable area
  cairo_set_source_rgb (cr, 0.1, 0.1, 0.1);

  const float np1x = px + (np_left / pg_width) * pwidth;
  const float np1y = py + (np_top / pg_height) * pheight;
  const float np2x = pright - (np_right / pg_width) * pwidth;
  const float np2y = pbottom - (np_bottom / pg_height) * pheight;

  // top-left
  cairo_move_to (cr, np1x-10, np1y);
  cairo_line_to (cr, np1x, np1y); cairo_line_to (cr, np1x, np1y-10);
  cairo_stroke (cr);

  // top-right
  // npy = p_y + (np_top / pg_height) * p_height;
  cairo_move_to (cr, np2x+10, np1y);
  cairo_line_to (cr, np2x, np1y); cairo_line_to (cr, np2x, np1y-10);
  cairo_stroke (cr);

  // bottom-left
  cairo_move_to (cr, np1x-10, np2y);
  cairo_line_to (cr, np1x, np2y); cairo_line_to (cr, np1x, np2y+10);
  cairo_stroke (cr);

  // bottom-right
  cairo_move_to (cr, np2x+10, np2y);
  cairo_line_to (cr, np2x, np2y); cairo_line_to (cr, np2x, np2y+10);
  cairo_stroke (cr);

  // clip to this area to ensure that the image won't be larger,
  // this is needed when using negative margin to enlarge the print

  cairo_rectangle (cr, np1x, np1y, np2x-np1x, np2y-np1y);
  cairo_clip(cr);

  cairo_set_source_rgb (cr, 0.77, 0.77, 0.77);
  cairo_rectangle (cr, ax, ay, awidth, aheight);
  cairo_fill (cr);
}

static void _print_setup_initial_image(dt_print_t *prt)
{
  if(IS_NULL_PTR(prt->pinfo) || IS_NULL_PTR(prt->imgs)) return;

  int32_t imgid = prt->pending_imgid;
  if(imgid <= UNKNOWN_IMAGE) imgid = prt->imgs->imgid_to_load;
  if(imgid <= UNKNOWN_IMAGE) imgid = dt_selection_get_first_id(darktable.selection);
  if(imgid <= UNKNOWN_IMAGE) imgid = dt_view_active_images_get_first();

  if(imgid <= UNKNOWN_IMAGE) return;
  if(prt->pinfo->printer.resolution <= 0 || prt->pinfo->paper.width <= 0.0f || prt->pinfo->paper.height <= 0.0f)
    return;
  if(!dt_isfinite(prt->imgs->screen.page.width) || !dt_isfinite(prt->imgs->screen.page.height)
     || !dt_isfinite(prt->imgs->screen.print_area.width) || !dt_isfinite(prt->imgs->screen.print_area.height)
     || prt->imgs->screen.page.width <= 1.0f || prt->imgs->screen.page.height <= 1.0f
     || prt->imgs->screen.print_area.width <= 1.0f || prt->imgs->screen.print_area.height <= 1.0f)
    return;

  if(prt->imgs->count > 0)
  {
    const dt_image_box *box = &prt->imgs->box[0];
    if(box->imgid > UNKNOWN_IMAGE && dt_isfinite(box->pos.x) && dt_isfinite(box->pos.y)
       && dt_isfinite(box->pos.width) && dt_isfinite(box->pos.height) && box->pos.width > 0.0f
       && box->pos.height > 0.0f)
      return;

    // Do not keep a half-built startup box around. If the view tried to
    // initialize before the page geometry existed, relative coordinates can be
    // invalid and the box would never recover on later redraws.
    for(int k = 0; k < prt->imgs->count; k++)
      dt_printing_clear_box(&prt->imgs->box[k]);
    prt->imgs->count = 0;
    prt->imgs->motion_over = -1;
  }

  float page_width = prt->pinfo->paper.width;
  float page_height = prt->pinfo->paper.height;
  if(prt->pinfo->page.landscape)
  {
    page_width = prt->pinfo->paper.height;
    page_height = prt->pinfo->paper.width;
  }

  // Build the first full-page box only once the page and print area are known.
  // This keeps the shared model free of NaN relative coordinates and lets the
  // view own the initial preview lifecycle without a delayed lib-side reload.
  dt_printing_setup_box(prt->imgs, 0, prt->imgs->screen.page.x, prt->imgs->screen.page.y,
                        prt->imgs->screen.page.width, prt->imgs->screen.page.height);
  dt_printing_setup_page(prt->imgs, page_width, page_height, prt->pinfo->printer.resolution);
  dt_printing_setup_image(prt->imgs, 0, imgid, 100, 100, ALIGNMENT_CENTER);
  prt->imgs->imgid_to_load = -1;
  prt->pending_imgid = -1;
}

void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  dt_print_t *prt = (dt_print_t *)self->data;

  // clear the current surface
  dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_PRINT_BG);
  cairo_paint(cri);

  // Draw the page first so the image fetcher paints into the same clipped print
  // area as the legacy synchronous path did.
  expose_print_page(self, cri, width_i, height_i, pointerx, pointery);
  _print_setup_initial_image(prt);

  gboolean busy = FALSE;

  for(int k = 0; k < prt->imgs->count; k++)
  {
    dt_image_box *img = &prt->imgs->box[k];
    if(img->imgid == UNKNOWN_IMAGE) continue;

    dt_printing_setup_image(prt->imgs, k, img->imgid, 100, 100, img->alignment);
    const int screen_width = ceilf(img->screen.width);
    const int screen_height = ceilf(img->screen.height);
    if(screen_width < 2 || screen_height < 2) continue;

    const dt_view_surface_value_t res =
      dt_view_image_get_surface_async(&prt->screen_fetchers[k], img->imgid, screen_width, screen_height,
                                      &prt->screen_surfaces[k], dt_ui_center(darktable.gui->ui),
                                      DT_THUMBTABLE_ZOOM_FIT);

    if(res != DT_VIEW_SURFACE_OK)
    {
      busy = TRUE;
      continue;
    }

    cairo_surface_t *surf = prt->screen_surfaces[k];
    if(IS_NULL_PTR(surf)) continue;

    const int surf_width = cairo_image_surface_get_width(surf);
    const int surf_height = cairo_image_surface_get_height(surf);
    double sx = 1.0, sy = 1.0;
    cairo_surface_get_device_scale(surf, &sx, &sy);
    const double logical_width = surf_width / sx;
    const double logical_height = surf_height / sy;
    const double x_offset = img->screen.x + (img->screen.width - logical_width) / 2.0;
    const double y_offset = img->screen.y + (img->screen.height - logical_height) / 2.0;

    img->img_width = roundf(logical_width);
    img->img_height = roundf(logical_height);
    img->dis_width = img->img_width;
    img->dis_height = img->img_height;

    cairo_save(cri);
    cairo_set_source_surface(cri, surf, x_offset, y_offset);
    cairo_paint(cri);
    cairo_restore(cri);
  }

  if(busy && !prt->busy) dt_control_log_busy_enter();
  if(!busy && prt->busy) dt_control_log_busy_leave();
  prt->busy = busy;
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  const dt_print_t *prt = (dt_print_t *)self->data;

  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of first opened image.
  const int32_t mouse_over_id = dt_control_get_mouse_over_id();

  if(prt->imgs->count == 1 && mouse_over_id != prt->imgs->box[0].imgid)
  {
    dt_control_set_mouse_over_id(prt->imgs->box[0].imgid);
  }
  else if(prt->imgs->count > 1)
  {
    const int bidx = dt_printing_get_image_box(prt->imgs, x, y);
    if(bidx == -1)
      dt_control_set_mouse_over_id(-1);
    else if(mouse_over_id != prt->imgs->box[bidx].imgid)
    {
      dt_control_set_mouse_over_id(prt->imgs->box[bidx].imgid);
    }
  }
}

int key_pressed(dt_view_t *self, GdkEventKey *event)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  switch(event->keyval)
  {
    case GDK_KEY_Escape:
      dt_ctl_switch_mode_to("lighttable");
      return TRUE;
    default:
      break;
  }

  return FALSE;
}

int try_enter(dt_view_t *self)
{
  dt_print_t *prt = (dt_print_t*)self->data;
  g_list_free(prt->incoming_selection);
  prt->incoming_selection = dt_selection_get_list(darktable.selection);

  //  now check that there is at least one selected image

  const int32_t imgid = prt->incoming_selection ? GPOINTER_TO_INT(prt->incoming_selection->data)
                                                : dt_selection_get_first_id(darktable.selection);

  if(imgid < 0)
  {
    // fail :(
    dt_control_log(_("no image to open !"));
    return 1;
  }

  // this loads the image from db if needed:
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  // get image and check if it has been deleted from disk first!

  char imgfilename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(img->id,  imgfilename,  sizeof(imgfilename),  &from_cache, __FUNCTION__);
  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image `%s' is currently unavailable"), img->filename);
    dt_image_cache_read_release(darktable.image_cache, img);
    return 1;
  }
  // and drop the lock again.
  dt_image_cache_read_release(darktable.image_cache, img);

  // we need to setup the selected image
  prt->pending_imgid = imgid;
  if(prt->imgs) prt->imgs->imgid_to_load = imgid;

  return 0;
}

void enter(dt_view_t *self)
{
  dt_print_t *prt=(dt_print_t*)self->data;

  dt_accels_connect_accels(darktable.gui->accels);
  dt_accels_connect_active_group(darktable.gui->accels, "print");

  dt_thumbtable_show(darktable.gui->ui->thumbtable_filmstrip);
  dt_thumbtable_update_parent(darktable.gui->ui->thumbtable_filmstrip);

  /* scroll filmstrip to the first selected image */
  int32_t startup_imgid = prt->pending_imgid;
  if(startup_imgid <= UNKNOWN_IMAGE) startup_imgid = prt->imgs->imgid_to_load;
  if(startup_imgid <= UNKNOWN_IMAGE) startup_imgid = dt_selection_get_first_id(darktable.selection);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_view_print_filmstrip_activate_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_FILMSTRIP_DRAG_BEGIN,
                            G_CALLBACK(_view_print_filmstrip_drag_begin_callback), self);

  dt_gui_refocus_center();

  GtkWidget *widget = dt_ui_center(darktable.gui->ui);

  gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_ALL,
                    target_list_all, n_targets_all, GDK_ACTION_MOVE);
  g_signal_connect(widget, "drag-data-received", G_CALLBACK(_drag_and_drop_received), self);
  g_signal_connect(widget, "drag-motion", G_CALLBACK(_drag_motion_received), self);

  dt_control_set_mouse_over_id(startup_imgid);
  dt_control_set_keyboard_over_id(startup_imgid);
  prt->last_selected = startup_imgid;
  g_idle_add((GSourceFunc)dt_thumbtable_scroll_to_selection, darktable.gui->ui->thumbtable_filmstrip);
  g_list_free(prt->incoming_selection);
  prt->incoming_selection = NULL;
}

void leave(dt_view_t *self)
{
  dt_print_t *prt=(dt_print_t*)self->data;
  dt_accels_disconnect_active_group(darktable.gui->accels);
  if(prt->busy) dt_control_log_busy_leave();
  prt->busy = FALSE;

  dt_thumbtable_hide(darktable.gui->ui->thumbtable_filmstrip);

  /* disconnect from filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                               G_CALLBACK(_view_print_filmstrip_activate_callback),
                               (gpointer)self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                               G_CALLBACK(_view_print_filmstrip_drag_begin_callback),
                               (gpointer)self);

  dt_printing_clear_boxes(prt->imgs);
  for(int k = 0; k < MAX_IMAGE_PER_PAGE; k++)
    dt_view_image_surface_fetcher_invalidate(&prt->screen_fetchers[k], &prt->screen_surfaces[k]);
//  g_signal_disconnect(widget, "drag-data-received", G_CALLBACK(_drag_and_drop_received));
//  g_signal_disconnect(widget, "drag-motion", G_CALLBACK(_drag_motion_received));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
