/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"

#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define DT_NAVIGATION_INSET 5

typedef struct dt_lib_navigation_t
{
  int dragging;
  int zoom_w, zoom_h;
} dt_lib_navigation_t;

typedef enum dt_lib_zoom_t
{
  LIB_ZOOM_SMALL = 0,
  LIB_ZOOM_FIT,
  LIB_ZOOM_25,
  LIB_ZOOM_50,
  LIB_ZOOM_100,
  LIB_ZOOM_200,
  LIB_ZOOM_400,
  LIB_ZOOM_800,
  LIB_ZOOM_1600,
  LIB_ZOOM_LAST
} dt_lib_zoom_t;


/* expose function for navigation module */
static gboolean _lib_navigation_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data);
/* motion notify callback handler*/
static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                       gpointer user_data);
/* button press callback */
static gboolean _lib_navigation_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                      gpointer user_data);
/* button release callback */
static gboolean _lib_navigation_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                        gpointer user_data);
/* leave notify callback */
static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                      gpointer user_data);

/* helper function for position set */
static void _lib_navigation_set_position(struct dt_lib_module_t *self, double x, double y, int wd, int ht);

const char *name(struct dt_lib_module_t *self)
{
  return _("navigation");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}


static void _lib_navigation_control_redraw_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_control_queue_redraw_widget(self->widget);
}


void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)g_malloc0(sizeof(dt_lib_navigation_t));
  self->data = (void *)d;

  /* create drawingarea */
  self->widget = gtk_drawing_area_new();
  gtk_widget_set_events(self->widget, GDK_EXPOSURE_MASK | GDK_ENTER_NOTIFY_MASK
                                      | GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK
                                      | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK);

  /* connect callbacks */
  gtk_widget_set_app_paintable(self->widget, TRUE);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_lib_navigation_draw_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-press-event",
                   G_CALLBACK(_lib_navigation_button_press_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-release-event",
                   G_CALLBACK(_lib_navigation_button_release_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "motion-notify-event",
                   G_CALLBACK(_lib_navigation_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "leave-notify-event",
                   G_CALLBACK(_lib_navigation_leave_notify_callback), self);

  /* set size of navigation draw area */
  gtk_widget_set_size_request(self->widget, -1, 175);
  gtk_widget_set_name(GTK_WIDGET(self->widget), "navigation-module");

  /* connect a redraw callback to control draw all and preview pipe finish signals */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_lib_navigation_control_redraw_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_NAVIGATION_REDRAW,
                            G_CALLBACK(_lib_navigation_control_redraw_callback), self);

  darktable.lib->proxy.navigation.module = self;
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect from signal */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_navigation_control_redraw_callback), self);

  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_navigation_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_develop_t *dev = darktable.develop;
  if(!dev->preview_pipe->output_backbuf || dev->image_storage.id != dev->preview_pipe->output_imgid)
    return TRUE;

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;

  /* get the current style */
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  /* draw navigation image if available */
  dt_pthread_mutex_t *mutex = &dev->preview_pipe->backbuf_mutex;
  dt_pthread_mutex_lock(mutex);

  cairo_save(cr);

  const int wd = dev->preview_pipe->output_backbuf_width;
  const int ht = dev->preview_pipe->output_backbuf_height;
  const float scale = fminf(width / (float)wd, height / (float)ht);

  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
  cairo_surface_t *surface
      = cairo_image_surface_create_for_data(dev->preview_pipe->output_backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);

  cairo_translate(cr, width / 2., height / 2.);
  cairo_scale(cr, scale, scale);
  cairo_translate(cr, -wd / 2., -ht / 2.);

  cairo_rectangle(cr, 0, 0, wd, ht);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_fill(cr);

  // draw box where we are
  // avoid numerical instability for small resolutions:
  double h, w;
  if(dev->scaling > 1.f)
  {
    // Add a dark overlay on the picture to make it fade
    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_fill(cr);

    // Repaint the original image in the area of interest
    cairo_set_source_surface(cr, surface, 0, 0);

    double roi_w = width / dev->scaling;
    double roi_h = height / dev->scaling;
    double roi_x = dev->x * width;
    double roi_y = dev->y * height;

    // Dig a hole into the overlay for the ROI
    cairo_rectangle(cr, roi_x - roi_w / 2., roi_y - roi_h / 2., roi_w + 2, roi_h + 2);
    cairo_clip_preserve(cr);
    cairo_fill_preserve(cr);

    // Paint the external border in black
    cairo_set_source_rgb(cr, 0., 0., 0.);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
    cairo_stroke(cr);

    // Paint the internal border in white
    cairo_set_source_rgb(cr, 1., 1., 1.);
    cairo_rectangle(cr, roi_x - roi_w / 2., roi_y - roi_h / 2., roi_w, roi_h);
    cairo_stroke(cr);
  }

  cairo_restore(cr);

  if(dev->scaling > 1.f)
  {
    /* Zoom % */
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    layout = pango_cairo_create_layout(cr);
    const float fontsize = DT_PIXEL_APPLY_DPI(14);
    pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    cairo_translate(cr, 0, height);
    cairo_set_source_rgba(cr, 1., 1., 1., 0.5);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    char zoomline[5];
    snprintf(zoomline, sizeof(zoomline), "%.0f%%", dev->scaling * dev->natural_scale * 100);

    pango_layout_set_text(layout, zoomline, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    h = d->zoom_h = ink.height;
    w = d->zoom_w = ink.width;

    cairo_move_to(cr, width - w - h * 1.1 - ink.x, - fontsize);

    cairo_save(cr);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));

    GdkRGBA *color;
    gtk_style_context_get(context, gtk_widget_get_state_flags(widget), "background-color", &color, NULL);

    gdk_cairo_set_source_rgba(cr, color);
    pango_cairo_layout_path(cr, layout);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
    cairo_fill(cr);
    cairo_restore(cr);

    gdk_rgba_free(color);
    pango_font_description_free(desc);
    g_object_unref(layout);

  }
  else
  {
    // draw the zoom-to-fit icon
    cairo_translate(cr, 0, height);
    cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);

    static int font_height = -1;
    if(font_height == -1)
    {
      PangoLayout *layout;
      PangoRectangle ink;
      PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      layout = pango_cairo_create_layout(cr);
      pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(14) * PANGO_SCALE);
      pango_layout_set_font_description(layout, desc);
      pango_layout_set_text(layout, "100%", -1); // dummy text, just to get the height
      pango_layout_get_pixel_extents(layout, &ink, NULL);
      font_height = ink.height;
      pango_font_description_free(desc);
      g_object_unref(layout);
    }

    h = d->zoom_h = font_height;
    w = h * 1.5;
    float sp = h * 0.6;
    d->zoom_w = w + sp;

    cairo_move_to(cr, width - w - h - sp, -1.0 * h);
    cairo_rectangle(cr, width - w - h - sp, -1.0 * h, w, h);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_fill(cr);

    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2));

    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_move_to(cr, width - w * 0.8 - h - sp, -1.0 * h);
    cairo_line_to(cr, width - w - h - sp, -1.0 * h);
    cairo_line_to(cr, width - w - h - sp, -0.7 * h);
    cairo_stroke(cr);
    cairo_move_to(cr, width - w - h - sp, -0.3 * h);
    cairo_line_to(cr, width - w - h - sp, 0);
    cairo_line_to(cr, width - w * 0.8 - h - sp, 0);
    cairo_stroke(cr);
    cairo_move_to(cr, width - w * 0.2 - h - sp, 0);
    cairo_line_to(cr, width - h - sp, 0);
    cairo_line_to(cr, width - h - sp, -0.3 * h);
    cairo_stroke(cr);
    cairo_move_to(cr, width - h - sp, -0.7 * h);
    cairo_line_to(cr, width - h - sp, -1.0 * h);
    cairo_line_to(cr, width - w * 0.2 - h - sp, -1.0 * h);
    cairo_stroke(cr);
  }

  cairo_move_to(cr, width - 0.95 * h, -0.9 * h);
  cairo_line_to(cr, width - 0.05 * h, -0.9 * h);
  cairo_line_to(cr, width - 0.5 * h, -0.1 * h);
  cairo_fill(cr);
  cairo_surface_destroy(surface);

  dt_pthread_mutex_unlock(mutex);

  /* blit memsurface into widget */
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

void _lib_navigation_set_position(dt_lib_module_t *self, double x, double y, int wd, int ht)
{
  dt_develop_t *dev = darktable.develop;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;

  if(!(dev && d->dragging && dev->scaling > 1.f)) return;

  // Correct widget coordinates for margins
  const float width = wd - 2.f * DT_NAVIGATION_INSET;
  const float height = ht - 2.f * DT_NAVIGATION_INSET;
  x -= DT_NAVIGATION_INSET;
  y -= DT_NAVIGATION_INSET;

  // Commit relative coordinates of the ROI center
  dev->x = x / width;
  dev->y = y / height;

  /* redraw myself */
  gtk_widget_queue_draw(self->widget);

  /* redraw pipe */
  dt_dev_invalidate_zoom(darktable.develop);
  dt_control_queue_redraw_center();
}

static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                       gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  _lib_navigation_set_position(self, event->x, event->y, allocation.width, allocation.height);
  return TRUE;
}

static void _zoom_preset_change(dt_lib_zoom_t zoom)
{
  // dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_develop_t *dev = darktable.develop;
  if(!dev) return;

  switch(zoom)
  {
    case LIB_ZOOM_SMALL:
      dev->scaling = 0.25;
      break;
    case LIB_ZOOM_FIT:
    default:
      dev->scaling = dev->natural_scale;
      break;
    case LIB_ZOOM_25:
      dev->scaling = 0.25;
      break;
    case LIB_ZOOM_50:
      dev->scaling = 0.50;
      break;
    case LIB_ZOOM_100:
      dev->scaling = 1.;
      break;
    case LIB_ZOOM_200:
      dev->scaling = 2.;
      break;
    case LIB_ZOOM_400:
      dev->scaling = 4.;
      break;
    case LIB_ZOOM_800:
      dev->scaling = 8.;
      break;
    case LIB_ZOOM_1600:
      dev->scaling = 16.;
      break;
  }

  // Actual pixelpipe scaling is dev->scaling * dev->natural_scale,
  // where dev->natural_scale ensures the images fits within viewport
  dev->scaling /= dev->natural_scale;

  dt_dev_invalidate_zoom(dev);
  dt_control_queue_redraw();
  dt_dev_refresh_ui_images(dev);
}

static void _zoom_preset_callback(GtkButton *button, gpointer user_data)
{
  _zoom_preset_change(GPOINTER_TO_INT(user_data));
}

static gboolean _lib_navigation_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                      gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int w = allocation.width;
  int h = allocation.height;
  if(event->x >= w - DT_NAVIGATION_INSET - d->zoom_h - d->zoom_w
     && event->y >= h - DT_NAVIGATION_INSET - d->zoom_h)
  {
    // we show the zoom menu
    GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
    GtkWidget *item;

    item = gtk_menu_item_new_with_label(_("small"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_SMALL));
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("fit to screen"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_FIT));
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("25%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_25));
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("50%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_50));
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("100%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_100));
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("200%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_200));
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("400%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_400));
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("800%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_800));
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("1600%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_callback), GINT_TO_POINTER(LIB_ZOOM_1600));
    gtk_menu_shell_append(menu, item);

    dt_gui_menu_popup(GTK_MENU(menu), NULL, 0, 0);

    return TRUE;
  }
  d->dragging = 1;
  _lib_navigation_set_position(self, event->x, event->y, w, h);
  return TRUE;
}

static gboolean _lib_navigation_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                        gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;
  d->dragging = 0;
  dt_dev_refresh_ui_images(darktable.develop);

  return TRUE;
}

static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                      gpointer user_data)
{
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
