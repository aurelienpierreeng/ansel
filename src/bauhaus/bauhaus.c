/*
    This file is part of Ansel,
    Copyright (C) 2012-2021 darktable developers.
    Copyright (C) 2022-2023 Aurélien Pierre.

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
#include "common/calculator.h"
#include "common/darktable.h"
#include "common/math.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <math.h>
#include <strings.h>

#include <pango/pangocairo.h>

G_DEFINE_TYPE(DtBauhausWidget, dt_bh, GTK_TYPE_DRAWING_AREA)

enum
{
  // Sliders
  DT_ACTION_ELEMENT_VALUE = 0,
  DT_ACTION_ELEMENT_BUTTON = 1,
  DT_ACTION_ELEMENT_FORCE = 2, // set slider value beyond visible range if soft min/max
  DT_ACTION_ELEMENT_ZOOM = 3,

  // Combos
  DT_ACTION_ELEMENT_SELECTION = 0,
};

// INNER_PADDING is the horizontal space between slider and quad
// and vertical space between labels and slider baseline
#define INNER_PADDING DT_PIXEL_APPLY_DPI(4)
#define INTERNAL_PADDING 2. * INNER_PADDING

// WARNING
// A lot of GUI setters/getters functions used to have type checking on input widgets
// and silently returned early if the types were not ok (like trying to set a combobox using slider methods).
// This only hides programmers mistakes in a way that prevents the soft to crash,
// but it still leads to faulty GUI interactions and inconsistent widgets values that might be hard to spot.
// In november 2023, they got removed in order to fail explicitly and possibly crash.
// If that's not enough, we can always add assertions in the code.

#define DEBUG 0

// fwd declare
static gboolean dt_bauhaus_popup_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static gboolean dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static gboolean _widget_draw(GtkWidget *widget, cairo_t *crf);
static gboolean _widget_scroll(GtkWidget *widget, GdkEventScroll *event);
static gboolean _widget_key_press(GtkWidget *widget, GdkEventKey *event);
static void _get_preferred_width(GtkWidget *widget, gint *minimum_size, gint *natural_size);
static void _style_updated(GtkWidget *widget);
static void dt_bauhaus_widget_accept(struct dt_bauhaus_widget_t *w, gboolean timeout);
static void dt_bauhaus_widget_reject(struct dt_bauhaus_widget_t *w);
static void _combobox_set(GtkWidget *widget, const int pos, gboolean timeout);

// !!! EXECUTIVE NOTE !!!
// Sizing and spacing need to be declared once only in getters/setters functions below.
// The rest of the code accesses those values only through the getters.
// Doxygen docstrings need to be added to explain what is computed, based on what.
// Code changes that recompute sizing or coordinates outside of getters/setters will be refused.

/**
 * @brief Update the box margin and padding properties of the widget w
 * by reading CSS context.
 *
 * @param w The widget to update and from which the CSS context is read.
 */
static void _margins_retrieve(struct dt_bauhaus_widget_t *w)
{
  if(!w->margin) w->margin = gtk_border_new();
  if(!w->padding) w->padding = gtk_border_new();
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(w));
  const GtkStateFlags state = gtk_widget_get_state_flags(GTK_WIDGET(w));
  gtk_style_context_get_margin(context, state, w->margin);
  gtk_style_context_get_padding(context, state, w->padding);
}

/**
 * @brief Get the total height of a GUI row containing a line of text + top and bottom padding.
 *
 * This applies to comboboxes list elements only. Sliders text lines have only bottom padding.
 *
 * @return float
 */
static float _bh_get_row_height()
{
  return darktable.bauhaus->line_height + INTERNAL_PADDING;
}

/**
 * @brief Get the width of the quad without padding
 *
 * @param w
 * @return double
 */
static double _widget_get_quad_width(struct dt_bauhaus_widget_t *w)
{
  if(w->show_quad)
    return darktable.bauhaus->quad_width;
  else
    return 0.;
}

/**
 * @brief Get the total width of the main Bauhaus widget area, accounting for padding and margins.
 *
 * @param w Pointer to the structure holding the widget attributes, aka the dt_bauhaus_widget_t
 * @param widget Actual GtkWidget to get allocation from. Can be NULL if it's the same as the Bauhaus widget.
 * @return double
 */
static double _widget_get_total_width(struct dt_bauhaus_widget_t *w, GtkWidget *widget)
{
  GtkWidget *box_reference = (widget) ? widget : GTK_WIDGET(w);
  GtkAllocation allocation;
  gtk_widget_get_allocation(box_reference, &allocation);
  return allocation.width - w->margin->left - w->margin->right - w->padding->left - w->padding->right;
}

/**
 * @brief Get the width of the main Bauhaus widget area (slider scale or combobox), accounting for quad space, padding and margins
 *
 * @param w Pointer to the structure holding the widget attributes, aka the dt_bauhaus_widget_t
 * @param widget Actual GtkWidget to get allocation from. Can be NULL if it's the same as the Bauhaus widget.
 * @param total_width Pointer where to write the total width of the widget, which is used in intermediate computations.
 * This will spare another call to `gtk_widget_get_allocation()` if both are needed. Can be NULL.
 * @return double
 */
static double _widget_get_main_width(struct dt_bauhaus_widget_t *w, GtkWidget *widget, double *total_width)
{
  const double tot_width = _widget_get_total_width(w, widget);
  if(total_width) *total_width = tot_width;
  return tot_width - _widget_get_quad_width(w) - 2. * INNER_PADDING;
}

/**
 * @brief Get the height of the main Bauhaus widget area (slider scale or combobox), that is the box allocation minus padding and margins.
 *
 * @param w Pointer to the structure holding the widget attributes, aka the dt_bauhaus_widget_t
 * @param widget Actual GtkWidget to get allocation from. Can be NULL if it's the same as the Bauhaus widget.
 * @return double
 */
static double _widget_get_main_height(struct dt_bauhaus_widget_t *w, GtkWidget *widget)
{
  GtkWidget *box_reference = (widget) ? widget : GTK_WIDGET(w);
  GtkAllocation allocation;
  gtk_widget_get_allocation(box_reference, &allocation);
  return allocation.height - w->margin->top - w->margin->bottom - w->padding->top - w->padding->bottom;
}

static double _get_combobox_height(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = (struct dt_bauhaus_widget_t *)widget;
  return w->margin->top + w->padding->top + w->margin->bottom + w->padding->bottom
         + darktable.bauhaus->line_height;
}

static double _get_slider_height(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = (struct dt_bauhaus_widget_t *)widget;
  return w->margin->top + w->padding->top + w->margin->bottom + w->padding->bottom + INNER_PADDING / 2.
         + 2. * darktable.bauhaus->border_width + darktable.bauhaus->line_height + darktable.bauhaus->marker_size;
}

static double _get_indicator_y_position()
{
  return darktable.bauhaus->line_height + INNER_PADDING + darktable.bauhaus->baseline_size / 2.0f;
}

static double _get_slider_bar_height()
{
  // Total height of the text label + slider baseline, discarding padding
  return darktable.bauhaus->line_height + INNER_PADDING + darktable.bauhaus->baseline_size;

}

static double _get_combobox_popup_height(struct dt_bauhaus_widget_t *w)
{
  dt_iop_module_t *module = (dt_iop_module_t *)(w->module);
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  // Need to run the populating callback first for dynamically-populated ones.
  if(d->populate) d->populate(GTK_WIDGET(w), &module);
  if(!d->entries->len) return 0.;

  int num_lines = d->entries->len;

  // Add an extra sit for user keyboard input if any
  if(darktable.bauhaus->keys_cnt > 0) num_lines += 1;

  return num_lines * _bh_get_row_height();
}


/**
 * @brief Translate in-place the cursor coordinates within the widget or popup according to padding and margin, so
 * x = 0 is mapped to the starting point of the slider.
 *
 * @param x Cursor coordinate x
 * @param y Cursor coordinate y
 * @param w Widget
 */
static void _translate_cursor(double *x, double *y, struct dt_bauhaus_widget_t *const w)
{
  *x -= w->margin->left + w->padding->left;
  *y -= w->margin->top + w->padding->top;
}

// Convenience state for cursor position over widget
typedef enum _bh_active_region_t
{
  BH_REGION_OUT = 0, // we are outside the padding box
  BH_REGION_MAIN,    // we are on the slider scale or combobox label/value, aka out of the quad button
  BH_REGION_QUAD,    // we are on the quad button
} _bh_active_region_t;

/**
 * @brief Check if we have user cursor over quad area or over the slider/main area, then correct cursor coordinates for widget padding and margin.
 * For sliders, it means that x = 0 is mapped to the origin of the scale.
 *
 * @param widget Bauhaus widget
 * @param event User event
 * @param x Initial coordinate x of the cursor. Will be corrected in-place for margin and padding.
 * @param y Initial coordinate y of the cursor. Will be corrected in-place for margin and padding.
 * @param width Return pointer for the main width. Can be NULL. Caller owns the memory and is responsible for freeing it.
 * @param popup Pointer to the Gtk window for the popup if any. Can be NULL. Height is computed from there if defined.
 * @return _bh_active_region_t the region being selected, or BH_REGION_OUT (aka 0) if the cursor is outside the padding box (inactive region).
 */
static _bh_active_region_t _bh_get_active_region(GtkWidget *widget, double *x, double *y, double *width, GtkWidget *popup)
{
  struct dt_bauhaus_widget_t *const w = DT_BAUHAUS_WIDGET(widget);

  // The widget to use as a reference to fetch allocation and compute sizes
  GtkWidget *box_reference = (popup) ? popup : widget;
  double total_width;
  const double main_width = _widget_get_main_width(w, box_reference, &total_width);
  const double main_height = _widget_get_main_height(w, box_reference);

  if(width) *width = main_width;
  _translate_cursor(x, y, w);

  // Check if we are within popup frame
  if(*y < 0. || *y > main_height || *x < 0. || *x > total_width)
    return BH_REGION_OUT;

  // Check where we are horizontally
  if(*x <= main_width + INTERNAL_PADDING)
    return BH_REGION_MAIN;
  else
    return BH_REGION_QUAD;

  return BH_REGION_OUT;
}

/**
 * @brief Round a slider numeric value to the number of digits specified in the widget `w`.
 *
 * @param w
 * @param x Value to round.
 * @return float
 */
static float _bh_round_to_n_digits(const struct dt_bauhaus_widget_t *const w, float x)
{
  const dt_bauhaus_slider_data_t *const d = &w->data.slider;
  const float factor = (float)ipow(10, d->digits);
  return roundf(x * factor) / factor;
}

/**
 * @brief Return the minimum representable value step, for current UI scaling factor and number of digits.
 *
 * @param w
 * @return float
 */
static float _bh_slider_get_min_step(const struct dt_bauhaus_widget_t *const w)
{
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->factor * (float)ipow(10, d->digits);
}

static double _bh_slider_get_scale(struct dt_bauhaus_widget_t *w)
{
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return 10.0 / (_bh_slider_get_min_step(w) * (d->max - d->min));
}

static void _bh_combobox_get_hovered_entry()
{
  if(darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX)
  {
    // Mark which combobox entry is active
    dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
    d->hovered = (int)floorf(darktable.bauhaus->mouse_y / _bh_get_row_height());
  }
}

static _bh_active_region_t _popup_coordinates(GtkWidget *widget, const double x_root, const double y_root, double *event_x, double *event_y)
{
  // Because the popup widget is a floating window, it keeps capturing motion events even if they don't
  // overlap it. In those events, (x, y) coordinates are expressed in the space of the hovered third-party widget,
  // meaning their coordinates will seem ok from here (right range regarding height/width of widget) but will belong to something else.
  // We need to grab absolute coordinates in the main window space to ensure we overlay the widget popup.
  gint wx, wy;
  GdkWindow *window = gtk_widget_get_window(darktable.bauhaus->popup_window);
  gdk_window_get_origin(window, &wx, &wy);
  *event_x = x_root - (double)wx;
  *event_y = y_root - (double)wy;
  return _bh_get_active_region(GTK_WIDGET(darktable.bauhaus->current), event_x, event_y, NULL, widget);
}

void bauhaus_request_focus(struct dt_bauhaus_widget_t *w)
{
  if(w->module && w->module->type == DT_ACTION_TYPE_IOP_INSTANCE)
    dt_iop_request_focus((dt_iop_module_t *)w->module);

  gtk_widget_grab_focus(GTK_WIDGET(w));
  gtk_widget_set_state_flags(GTK_WIDGET(w), GTK_STATE_FLAG_FOCUSED, TRUE);

  // Scroll focus needs to be managed separately from Gtk focus
  // because of Gtk notebooks (tabs): Gtk gives focus automatically to the first
  // notebook child, which is not what we want for scroll event capture.
  darktable.gui->has_scroll_focus = GTK_WIDGET(w);
}

static void _combobox_next_sensitive(struct dt_bauhaus_widget_t *w, int delta)
{
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  int new_pos = d->active;
  int step = delta > 0 ? 1 : -1;
  int cur = new_pos + step;
  while(delta && cur >= 0 && cur < d->entries->len)
  {
    dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, cur);
    if(entry->sensitive)
    {
      new_pos = cur;
      delta -= step;
    }
    cur += step;
  }
  d->hovered = new_pos;
  _combobox_set(GTK_WIDGET(w), new_pos, TRUE);
}

static dt_bauhaus_combobox_entry_t *new_combobox_entry(const char *label, dt_bauhaus_combobox_alignment_t alignment,
                                                       gboolean sensitive, void *data, void (*free_func)(void *))
{
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)calloc(1, sizeof(dt_bauhaus_combobox_entry_t));
  entry->label = g_strdup(label);
  entry->alignment = alignment;
  entry->sensitive = sensitive;
  entry->data = data;
  entry->free_func = free_func;
  return entry;
}

static void free_combobox_entry(gpointer data)
{
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)data;
  g_free(entry->label);
  if(entry->free_func)
    entry->free_func(entry->data);
  free(entry);
}

static GdkRGBA *default_color_assign()
{
  // helper to initialize a color pointer with red color as a default
  GdkRGBA color = {.red = 1.0f, .green = 0.0f, .blue = 0.0f, .alpha = 1.0f};
  return gdk_rgba_copy(&color);
}

// Vertical alignment of text in its bounding box
typedef enum _bh_valign_t
{
  BH_ALIGN_TOP = 0,
  BH_ALIGN_BOTTOM = 1,
  BH_ALIGN_MIDDLE = 2
} _bh_valign_t;

// Horizontal alignment of text in its bounding box
typedef enum _bh_halign_t
{
  BH_ALIGN_LEFT = 0,
  BH_ALIGN_RIGHT = 1,
  BH_ALIGN_CENTER = 2
} _bh_halign_t;

/**
 * @brief Display text aligned in a bounding box, with pseudo-classes properties handled, and optional background color.
 *
 * @param w The current widget
 * @param context Gtk CSS context
 * @param cr Cairo drawing object
 * @param bounding_box The bounding box in which the text should fit.
 * @param text The text content to display.
 * @param halign Horizontal alignment within the bounding box
 * @param valign Vertical alignment within the bounding box
 * @param ellipsize Pango ellipsization strategy, used only if text overflows its bounding box.
 * @param bg_color Background color to paint in the bounding box. Can be NULL.
 * @param width Pointer where text spanning width will be returned in Cairo units. Can be NULL.
 * @param height Pointer where text spanning height will be returned in Cairo units. Can be NULL.
 * @param ignore_pseudo_classes Disregard styling done in pseudo-classes and use the normal style.
 */
static void show_pango_text(struct dt_bauhaus_widget_t *w, GtkStyleContext *context,
                            cairo_t *cr, GdkRectangle *bounding_box,
                            const char *text,
                            _bh_halign_t halign, _bh_valign_t valign,
                            PangoEllipsizeMode ellipsize,
                            GdkRGBA *bg_color, float *width, float *height,
                            gboolean ignore_pseudo_classes)
{
  if(text == NULL) return;

  // Prepare context and font properties
  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font_desc = NULL;
  GtkStateFlags state = (ignore_pseudo_classes) ? GTK_STATE_FLAG_NORMAL
                                                : gtk_widget_get_state_flags(GTK_WIDGET(w));
  gtk_style_context_get(context, state, "font", &font_desc, NULL);
  pango_layout_set_font_description(layout, font_desc);

  // FIXME: Is that still needed in 2023 ?
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  // Set the actual text
  pango_layout_set_text(layout, text, -1);

  // Record Pango sizes, convert them to Cairo units and return them
  int pango_width;
  int pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  double text_width = (double)pango_width / PANGO_SCALE;
  double text_height = (double)pango_height / PANGO_SCALE;
  if(width) *width = text_width;
  if(height) *height = text_height;

  // Handle bounding box overflow if any
  if(text_width > bounding_box->width)
  {
    pango_layout_set_ellipsize(layout, ellipsize);
    pango_layout_set_width(layout, (int)(PANGO_SCALE * bounding_box->width));
    text_width = bounding_box->width;
    if(width) *width = text_width;
  }

  // Paint background color if any - useful to highlight elements in popup list
  if(bg_color)
  {
    cairo_save(cr);
    cairo_rectangle(cr, bounding_box->x, bounding_box->y, bounding_box->width, bounding_box->height);
    cairo_set_source_rgba(cr, bg_color->red, bg_color->green, bg_color->blue, bg_color->alpha);
    cairo_fill(cr);
    cairo_restore(cr);
  }

  // Compute the coordinates of the top-left corner as to ensure proper alignment
  // in bounding box given the dimensions of the label.
  double x = 0;
  switch(halign)
  {
    case BH_ALIGN_CENTER:
      x = bounding_box->x + bounding_box->width / 2. - text_width / 2.;
      break;
    case BH_ALIGN_RIGHT:
      x = bounding_box->x + bounding_box->width - text_width;
      break;
    case BH_ALIGN_LEFT:
    default:
      x = bounding_box->x;
      break;
  }

  double y = 0;
  switch(valign)
  {
    case BH_ALIGN_MIDDLE:
      y = bounding_box->y + bounding_box->height / 2. - text_height / 2.;
      break;
    case BH_ALIGN_BOTTOM:
      y = bounding_box->y + bounding_box->height - text_height;
      break;
    case BH_ALIGN_TOP:
    default:
      y = bounding_box->y;
      break;
  }

  // Actually (finally) draw everything in place
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);

  // Cleanup
  pango_font_description_free(font_desc);
  g_object_unref(layout);
}

static void dt_bauhaus_slider_set_normalized(struct dt_bauhaus_widget_t *w, float pos, gboolean raise, gboolean timeout);

static double get_slider_line_offset(const double pos, const double scale, const double x, double y, const double line_height)
{
  // handle linear startup and rescale y to fit the whole range again
  // Note : all inputs are in relative coordinates, except pos
  float offset = 0.f;
  if(y < line_height)
  {
    offset = x - pos;
  }
  else
  {
    // Renormalize y coordinates below the baseline
    y = (y - line_height) / (1.0 - line_height);
    offset = (x - sqf(y) * .5 - (1.0 - sqf(y)) * pos)
             / (.5 * sqf(y) / scale + (1.0 - sqf(y)));
  }
  // clamp to result in a [0,1] range:
  if(pos + offset > 1.0) offset = 1.0 - pos;
  if(pos + offset < 0.0) offset = -pos;
  return offset;
}

// draw a loupe guideline for the quadratic zoom in in the slider interface:
static void draw_slider_line(cairo_t *cr, const double pos, const double off, const double scale, const double width, const double height,
                             const double line_height, double line_width)
{
  // pos is normalized position [0,1], offset is on that scale.
  // ht is in pixels here
  const int steps = 128;
  const double corrected_height = (height - line_height);

  cairo_set_line_width(cr, line_width);
  cairo_move_to(cr, width * (pos + off), line_height);
  const double half_line_width = line_width / 2.;
  for(int j = 1; j < steps; j++)
  {
    const double y = (double)j / (double)(steps - 1);
    const double x = sqf(y) * .5f * (1.f + off / scale) + (1.0f - sqf(y)) * (pos + off);
    cairo_line_to(cr, x * width - half_line_width, line_height + y * corrected_height);
  }
}
// -------------------------------

static void _slider_zoom_range(struct dt_bauhaus_widget_t *w, float zoom)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  const float value = dt_bauhaus_slider_get(GTK_WIDGET(w));

  if(roundf(zoom) == 0.f)
  {
    d->min = d->soft_min;
    d->max = d->soft_max;
    dt_bauhaus_slider_set(GTK_WIDGET(w), value); // restore value (and move min/max again if needed)
    return;
  }

  // make sure current value still in zoomed range
  const float min_visible = _bh_slider_get_min_step(w);
  const float multiplier = exp2f(zoom / 2.f);
  const float new_min = value - multiplier * (value - d->min);
  const float new_max = value + multiplier * (d->max - value);
  if(new_min >= d->hard_min
      && new_max <= d->hard_max
      && new_max - new_min >= min_visible * 10)
  {
    d->min = new_min;
    d->max = new_max;
  }

  gtk_widget_queue_draw(GTK_WIDGET(w));
}

static void _slider_zoom_toast(struct dt_bauhaus_widget_t *w)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  gchar *min_text = dt_bauhaus_slider_get_text(GTK_WIDGET(w), d->factor > 0 ? d->min : d->max);
  gchar *max_text = dt_bauhaus_slider_get_text(GTK_WIDGET(w), d->factor > 0 ? d->max : d->min);
  gchar *text = g_strdup_printf(("\n[%s , %s]"), min_text, max_text);
  dt_action_widget_toast(w->module, GTK_WIDGET(w), text);
  g_free(text);
  g_free(min_text);
  g_free(max_text);
}

static gboolean dt_bauhaus_popup_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_y = 0;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX)
      _combobox_next_sensitive(darktable.bauhaus->current, delta_y);
    else
    {
      _slider_zoom_range(darktable.bauhaus->current, delta_y);
      gtk_widget_queue_draw(widget);
    }
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  struct dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  double event_x;
  double event_y;
  const _bh_active_region_t active = _popup_coordinates(widget, event->x_root, event->y_root, &event_x, &event_y);

#if DEBUG
  fprintf(stdout, "x: %i, y: %i, active: %i\n", (int)event_x, (int)event_y, active);
#endif

  if(active == BH_REGION_OUT) return FALSE;

  // Pass-on new cursor coordinates corrected for padding and margin
  // and start a redraw. Nothing else.
  darktable.bauhaus->mouse_x = event_x;
  darktable.bauhaus->mouse_y = event_y;

  if(w->type == DT_BAUHAUS_COMBOBOX)
  {
    _bh_combobox_get_hovered_entry();
    gtk_widget_queue_draw(darktable.bauhaus->popup_area);
  }
  else
  {
    dt_bauhaus_slider_data_t *d = &w->data.slider;
    const double main_height = _widget_get_main_height(w, widget);
    const float mouse_off = get_slider_line_offset(
        d->oldpos, _bh_slider_get_scale(w), darktable.bauhaus->mouse_x / _widget_get_main_width(w, NULL, NULL),
        darktable.bauhaus->mouse_y / main_height, _get_slider_bar_height() / main_height);

    if(d->is_dragging)
    {
      // On dragging (when holding a click), we commit intermediate values to pipeline for "realtime" preview
      dt_bauhaus_slider_set_normalized(w, d->oldpos + mouse_off, TRUE, TRUE);
    }
    else
    {
      // If not dragging, assume user just wants to take his time to fine-tune the value.
      d->pos = d->oldpos + mouse_off;
      gtk_widget_queue_draw(darktable.bauhaus->popup_area);
    }
  }

  return TRUE;
}

static gboolean dt_bauhaus_popup_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_NORMAL, TRUE);
  return TRUE;
}

static gboolean dt_bauhaus_popup_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;

  if(w && (w->type == DT_BAUHAUS_COMBOBOX) && (event->button == 1)
     && (event->time >= darktable.bauhaus->opentime + delay) && !darktable.bauhaus->hiding)
  {
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_ACTIVE, TRUE);

    dt_bauhaus_hide_popup();
  }
  else if(darktable.bauhaus->hiding)
  {
    dt_bauhaus_hide_popup();
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;

  if(event->button == 1)
  {
    if(darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX
       && event->time < darktable.bauhaus->opentime + delay)
    {
      // counts as double click, reset:
      const dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
      _combobox_set(GTK_WIDGET(darktable.bauhaus->current), d->defpos, FALSE);
      dt_bauhaus_widget_reject(darktable.bauhaus->current);
    }
    else
    {
      // only accept left mouse click.
      // coordinates are set in motion_notify, which also makes sure they are within the valid range.
      // problems appear with the cornercase where user didn't move the cursor since opening the popup.
      // aka we need to re-read coordinates here.
      double event_x;
      double event_y;
      const _bh_active_region_t active = _popup_coordinates(widget, event->x_root, event->y_root, &event_x, &event_y);

      if(active == BH_REGION_OUT)
      {
        dt_bauhaus_widget_reject(darktable.bauhaus->current);
        dt_bauhaus_hide_popup();
        return TRUE;
      }

      darktable.bauhaus->end_mouse_x = darktable.bauhaus->mouse_x = event_x;
      darktable.bauhaus->end_mouse_y = darktable.bauhaus->mouse_y = event_y;

      if(w->type == DT_BAUHAUS_SLIDER)
      {
        dt_bauhaus_slider_data_t *d = &w->data.slider;
        d->is_dragging = TRUE;

        // Trick to ensure new value ≠ d->pos (so we commit to pipeline), since
        // d->pos is used for uncommitted drawings.
        const float value = d->pos;
        d->pos = d->oldpos;
        dt_bauhaus_slider_set_normalized(w, value, TRUE, FALSE);
      }
      else
      {
        _bh_combobox_get_hovered_entry();
        dt_bauhaus_widget_accept(darktable.bauhaus->current, FALSE);
      }
    }
    darktable.bauhaus->hiding = TRUE;
  }
  else if(event->button == 2 && darktable.bauhaus->current->type == DT_BAUHAUS_SLIDER)
  {
    _slider_zoom_range(darktable.bauhaus->current, 0);
    gtk_widget_queue_draw(widget);
  }
  else
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    darktable.bauhaus->hiding = TRUE;
  }
  gtk_widget_set_state_flags(GTK_WIDGET(darktable.bauhaus->current), GTK_STATE_FLAG_FOCUSED, TRUE);
  return TRUE;
}

static void dt_bauhaus_window_show(GtkWidget *w, gpointer user_data)
{
  // Could grab the popup_area rather than popup_window, but if so
  // then popup_area would get all motion events including those
  // outside of the popup. This way the popup_area gets motion events
  // related to updating the popup, and popup_window gets all others
  // which would be the ones telling it to close the popup.
  gtk_grab_add(GTK_WIDGET(user_data));
}

static void dt_bh_init(DtBauhausWidget *class)
{
  // not sure if we want to use this instead of our code in *_new()
  // TODO: the common code from bauhaus_widget_init() could go here.
}

static gboolean _enter_leave(GtkWidget *widget, GdkEventCrossing *event)
{
  if(event->type == GDK_ENTER_NOTIFY)
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_PRELIGHT, FALSE);
  else
    gtk_widget_unset_state_flags(widget, GTK_STATE_FLAG_PRELIGHT);

  gtk_widget_queue_draw(widget);

  return FALSE;
}

static void _widget_finalize(GObject *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type == DT_BAUHAUS_SLIDER)
  {
    dt_bauhaus_slider_data_t *d = &w->data.slider;
    if(d->timeout_handle) g_source_remove(d->timeout_handle);
    free(d->grad_pos);
  }
  else
  {
    dt_bauhaus_combobox_data_t *d = &w->data.combobox;
    g_ptr_array_free(d->entries, TRUE);
    free(d->text);
  }
  g_free(w->section);
  gtk_border_free(w->margin);
  gtk_border_free(w->padding);

  G_OBJECT_CLASS(dt_bh_parent_class)->finalize(widget);
}

static void dt_bh_class_init(DtBauhausWidgetClass *class)
{
  darktable.bauhaus->signals[DT_BAUHAUS_VALUE_CHANGED_SIGNAL]
      = g_signal_new("value-changed", G_TYPE_FROM_CLASS(class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
  darktable.bauhaus->signals[DT_BAUHAUS_QUAD_PRESSED_SIGNAL]
      = g_signal_new("quad-pressed", G_TYPE_FROM_CLASS(class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);
  widget_class->draw = _widget_draw;
  widget_class->scroll_event = _widget_scroll;
  widget_class->key_press_event = _widget_key_press;
  widget_class->get_preferred_width = _get_preferred_width;
  widget_class->enter_notify_event = _enter_leave;
  widget_class->leave_notify_event = _enter_leave;
  widget_class->style_updated = _style_updated;
  G_OBJECT_CLASS(class)->finalize = _widget_finalize;
}

void dt_bauhaus_load_theme()
{
  darktable.bauhaus->line_height = 3;
  darktable.bauhaus->marker_size = 0.25f;

  GtkWidget *root_window = dt_ui_main_window(darktable.gui->ui);
  GtkStyleContext *ctx = gtk_style_context_new();
  GtkWidgetPath *path = gtk_widget_path_new();
  gtk_style_context_set_path(ctx, path);
  gtk_style_context_set_screen (ctx, gtk_widget_get_screen(root_window));

  gtk_style_context_lookup_color(ctx, "bauhaus_fg", &darktable.bauhaus->color_fg);
  gtk_style_context_lookup_color(ctx, "bauhaus_fg_insensitive", &darktable.bauhaus->color_fg_insensitive);
  gtk_style_context_lookup_color(ctx, "bauhaus_bg", &darktable.bauhaus->color_bg);
  gtk_style_context_lookup_color(ctx, "bauhaus_border", &darktable.bauhaus->color_border);
  gtk_style_context_lookup_color(ctx, "bauhaus_fill", &darktable.bauhaus->color_fill);
  gtk_style_context_lookup_color(ctx, "bauhaus_indicator_border", &darktable.bauhaus->indicator_border);

  gtk_style_context_lookup_color(ctx, "graph_bg", &darktable.bauhaus->graph_bg);
  gtk_style_context_lookup_color(ctx, "graph_exterior", &darktable.bauhaus->graph_exterior);
  gtk_style_context_lookup_color(ctx, "graph_border", &darktable.bauhaus->graph_border);
  gtk_style_context_lookup_color(ctx, "graph_grid", &darktable.bauhaus->graph_grid);
  gtk_style_context_lookup_color(ctx, "graph_fg", &darktable.bauhaus->graph_fg);
  gtk_style_context_lookup_color(ctx, "graph_fg_active", &darktable.bauhaus->graph_fg_active);
  gtk_style_context_lookup_color(ctx, "graph_overlay", &darktable.bauhaus->graph_overlay);
  gtk_style_context_lookup_color(ctx, "inset_histogram", &darktable.bauhaus->inset_histogram);
  gtk_style_context_lookup_color(ctx, "graph_red", &darktable.bauhaus->graph_colors[0]);
  gtk_style_context_lookup_color(ctx, "graph_green", &darktable.bauhaus->graph_colors[1]);
  gtk_style_context_lookup_color(ctx, "graph_blue", &darktable.bauhaus->graph_colors[2]);
  gtk_style_context_lookup_color(ctx, "colorlabel_red",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_RED]);
  gtk_style_context_lookup_color(ctx, "colorlabel_yellow",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_YELLOW]);
  gtk_style_context_lookup_color(ctx, "colorlabel_green",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_GREEN]);
  gtk_style_context_lookup_color(ctx, "colorlabel_blue",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_BLUE]);
  gtk_style_context_lookup_color(ctx, "colorlabel_purple",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_PURPLE]);

  PangoFontDescription *pfont = 0;
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "font", &pfont, NULL);

  // make sure we release previously loaded font
  if(darktable.bauhaus->pango_font_desc)
    pango_font_description_free(darktable.bauhaus->pango_font_desc);

  darktable.bauhaus->pango_font_desc = pfont;
  gtk_widget_path_free(path);

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 128);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, "M", -1);
  pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  int pango_width;
  int pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(cst);

  darktable.bauhaus->line_height = pango_height / PANGO_SCALE;
  darktable.bauhaus->quad_width = darktable.bauhaus->line_height;

  darktable.bauhaus->baseline_size = DT_PIXEL_APPLY_DPI(5); // absolute size in Cairo unit
  darktable.bauhaus->border_width = DT_PIXEL_APPLY_DPI(2); // absolute size in Cairo unit
  darktable.bauhaus->marker_size = pango_width / PANGO_SCALE * 0.8;
}

void dt_bauhaus_init()
{
  darktable.bauhaus = (dt_bauhaus_t *)calloc(1, sizeof(dt_bauhaus_t));
  darktable.bauhaus->keys_cnt = 0;
  darktable.bauhaus->current = NULL;
  darktable.bauhaus->popup_area = gtk_drawing_area_new();
  darktable.bauhaus->pango_font_desc = NULL;

  dt_bauhaus_load_theme();

  darktable.bauhaus->skip_accel = 1;

  // this easily gets keyboard input:
  // darktable.bauhaus->popup_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  // but this doesn't flicker, and the above hack with key input seems to work well.
  darktable.bauhaus->popup_window = gtk_window_new(GTK_WINDOW_POPUP);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(darktable.bauhaus->popup_window);
#endif
  // this is needed for popup, not for toplevel.
  // since popup_area gets the focus if we show the window, this is all
  // we need.

  gtk_window_set_resizable(GTK_WINDOW(darktable.bauhaus->popup_window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(darktable.bauhaus->popup_window), 260, 260);
  gtk_window_set_modal(GTK_WINDOW(darktable.bauhaus->popup_window), TRUE);

  // Needed for Wayland and Sway :
  gtk_window_set_transient_for(GTK_WINDOW(darktable.bauhaus->popup_window),
                               GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));

  gtk_window_set_decorated(GTK_WINDOW(darktable.bauhaus->popup_window), FALSE);
  gtk_window_set_attached_to(GTK_WINDOW(darktable.bauhaus->popup_window), NULL);

  // needed on macOS to avoid fullscreening the popup with newer GTK
  gtk_window_set_type_hint(GTK_WINDOW(darktable.bauhaus->popup_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

  gtk_container_add(GTK_CONTAINER(darktable.bauhaus->popup_window), darktable.bauhaus->popup_area);
  gtk_widget_set_hexpand(darktable.bauhaus->popup_area, TRUE);
  gtk_widget_set_vexpand(darktable.bauhaus->popup_area, TRUE);
  gtk_window_set_keep_above(GTK_WINDOW(darktable.bauhaus->popup_window), TRUE);
  gtk_window_set_gravity(GTK_WINDOW(darktable.bauhaus->popup_window), GDK_GRAVITY_STATIC);

  gtk_widget_set_can_focus(darktable.bauhaus->popup_area, TRUE);
  gtk_widget_add_events(darktable.bauhaus->popup_area, GDK_POINTER_MOTION_MASK
                                                       | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                       | GDK_KEY_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK
                                                       | darktable.gui->scroll_mask);

  GObject *window = G_OBJECT(darktable.bauhaus->popup_window), *area = G_OBJECT(darktable.bauhaus->popup_area);
  g_signal_connect(window, "show", G_CALLBACK(dt_bauhaus_window_show), area);
  g_signal_connect(area, "draw", G_CALLBACK(dt_bauhaus_popup_draw), NULL);
  g_signal_connect(area, "motion-notify-event", G_CALLBACK(dt_bauhaus_popup_motion_notify), NULL);
  g_signal_connect(area, "leave-notify-event", G_CALLBACK(dt_bauhaus_popup_leave_notify), NULL);
  g_signal_connect(area, "button-press-event", G_CALLBACK(dt_bauhaus_popup_button_press), NULL);
  g_signal_connect(area, "button-release-event", G_CALLBACK (dt_bauhaus_popup_button_release), NULL);
  g_signal_connect(area, "key-press-event", G_CALLBACK(dt_bauhaus_popup_key_press), NULL);
  g_signal_connect(area, "scroll-event", G_CALLBACK(dt_bauhaus_popup_scroll), NULL);
}

void dt_bauhaus_cleanup()
{
}

// fwd declare a few callbacks
static gboolean dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_slider_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);


static gboolean dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);

// end static init/cleanup
// =================================================



// common initialization
static void _bauhaus_widget_init(struct dt_bauhaus_widget_t *w, dt_iop_module_t *self)
{
  w->module = DT_ACTION(self);
  w->field = NULL;

  w->section = NULL;

  // no quad icon and no toggle button:
  w->quad_paint = 0;
  w->quad_paint_data = NULL;
  w->quad_toggle = 0;
  w->show_quad = TRUE;
  w->show_label = TRUE;

  gtk_widget_add_events(GTK_WIDGET(w), GDK_POINTER_MOTION_MASK
                                       | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                       | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                       | GDK_FOCUS_CHANGE_MASK
                                       | darktable.gui->scroll_mask);

  gtk_widget_set_can_focus(GTK_WIDGET(w), TRUE);
  dt_gui_add_class(GTK_WIDGET(w), "dt_bauhaus");
}

void dt_bauhaus_combobox_set_default(GtkWidget *widget, int def)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->defpos = def;
}

int dt_bauhaus_combobox_get_default(GtkWidget *widget)
{
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->defpos;
}

void dt_bauhaus_slider_set_hard_min(GtkWidget* widget, float val)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float current_position = dt_bauhaus_slider_get(widget);
  float desired_position = _bh_round_to_n_digits(w, val);
  d->min = MAX(d->min, d->hard_min);
  d->soft_min = MAX(d->soft_min, d->hard_min);

  if(desired_position > d->hard_max)
    dt_bauhaus_slider_set_hard_max(widget,val);

  if(current_position < desired_position)
    dt_bauhaus_slider_set(widget, desired_position);
  // else nothing : old position is the new position, just the bound changes
}

float dt_bauhaus_slider_get_hard_min(GtkWidget* widget)
{
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->hard_min;
}

void dt_bauhaus_slider_set_hard_max(GtkWidget* widget, float val)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float current_position = dt_bauhaus_slider_get(widget);
  float desired_position = _bh_round_to_n_digits(w, val);
  d->hard_max = desired_position;
  d->max = MIN(d->max, d->hard_max);
  d->soft_max = MIN(d->soft_max, d->hard_max);

  if(desired_position < d->hard_min)
    dt_bauhaus_slider_set_hard_min(widget, desired_position);

  if(current_position > desired_position)
    dt_bauhaus_slider_set(widget, desired_position);
  // else nothing : old position is the new position, just the bound changes
}

float dt_bauhaus_slider_get_hard_max(GtkWidget* widget)
{
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->hard_max;
}

void dt_bauhaus_slider_set_soft_min(GtkWidget* widget, float val)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float oldval = dt_bauhaus_slider_get(widget);
  d->min = d->soft_min = CLAMP(val, d->hard_min, d->hard_max);
  dt_bauhaus_slider_set(widget, oldval);
}

float dt_bauhaus_slider_get_soft_min(GtkWidget* widget)
{
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->soft_min;
}

void dt_bauhaus_slider_set_soft_max(GtkWidget* widget, float val)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float oldval = dt_bauhaus_slider_get(widget);
  d->max = d->soft_max = CLAMP(val, d->hard_min, d->hard_max);
  dt_bauhaus_slider_set(widget, oldval);
}

float dt_bauhaus_slider_get_soft_max(GtkWidget* widget)
{
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->soft_max;
}

void dt_bauhaus_slider_set_default(GtkWidget *widget, float def)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->defpos = def;
}

void dt_bauhaus_slider_set_soft_range(GtkWidget *widget, float soft_min, float soft_max)
{
  dt_bauhaus_slider_set_soft_min(widget, soft_min);
  dt_bauhaus_slider_set_soft_max(widget, soft_max);
}

float dt_bauhaus_slider_get_default(GtkWidget *widget)
{
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->defpos;
}


void dt_bauhaus_widget_set_label(GtkWidget *widget, const char *section, const char *label)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  memset(w->label, 0, sizeof(w->label)); // keep valgrind happy
  if(label)
  {
    g_strlcpy(w->label, _(label), sizeof(w->label));
    dt_capitalize_label(w->label);
  }

  if(section) w->section = g_strdup(_(section));

  if(w->module)
  {
    if(!darktable.bauhaus->skip_accel || w->module->type != DT_ACTION_TYPE_IOP_INSTANCE)
    {
      dt_action_t *ac = dt_action_define(w->module, section, label, widget, NULL);
      if(w->module->type != DT_ACTION_TYPE_IOP_INSTANCE) w->module = ac;
    }

    // if new bauhaus widget added to front of widget_list; move it to the back
    dt_iop_module_t *m = (dt_iop_module_t *)w->module;
    if(w->module->type == DT_ACTION_TYPE_IOP_INSTANCE &&
       w->field && m->widget_list && ((dt_action_target_t *)m->widget_list->data)->target == (gpointer)widget)
    {
      if(!m->widget_list_bh)
      {
        m->widget_list_bh = m->widget_list;
        if(m->widget_list->next)
        {
          GSList *last = g_slist_last(m->widget_list);
          last->next = m->widget_list;
          m->widget_list = m->widget_list->next;
          last->next->next = NULL;
        }
      }
      else
      {
        GSList *first = m->widget_list->next;
        m->widget_list->next = m->widget_list_bh->next;
        m->widget_list_bh->next = m->widget_list;
        m->widget_list = first;
      }
    }

    gtk_widget_queue_draw(GTK_WIDGET(w));
  }
}

const char* dt_bauhaus_widget_get_label(GtkWidget *widget)
{
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  return w->label;
}

void dt_bauhaus_widget_set_quad_paint(GtkWidget *widget, dt_bauhaus_quad_paint_f f, int paint_flags, void *paint_data)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_paint = f;
  w->quad_paint_flags = paint_flags;
  w->quad_paint_data = paint_data;
}

void dt_bauhaus_widget_set_field(GtkWidget *widget, gpointer field, dt_introspection_type_t field_type)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(*w->label)
    fprintf(stderr, "[dt_bauhaus_widget_set_field] bauhaus label '%s' set before field (needs to be after)\n", w->label);
  w->field = field;
  w->field_type = field_type;
}

// FIXME: IOP modules are parents, they shouldn't be handled from down here
// Wrong scope, bad API.
void dt_bauhaus_update_module(dt_iop_module_t *self)
{

#if DEBUG
  fprintf(stdout, "bauhaus update called for %s. %i widgets.\n", self->name(), g_slist_length(self->widget_list_bh));
#endif

  for(GSList *w = self->widget_list_bh; w; w = w->next)
  {
    dt_action_target_t *at = w->data;
    GtkWidget *widget = at->target;
    struct dt_bauhaus_widget_t *bhw = DT_BAUHAUS_WIDGET(widget);
    if(!bhw) continue;

    switch(bhw->type)
    {
      case DT_BAUHAUS_SLIDER:
        switch(bhw->field_type)
        {
          case DT_INTROSPECTION_TYPE_FLOAT:
            dt_bauhaus_slider_set(widget, *(float *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_INT:
            dt_bauhaus_slider_set(widget, *(int *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_USHORT:
            dt_bauhaus_slider_set(widget, *(unsigned short *)bhw->field);
            break;
          default:
            fprintf(stderr, "[dt_bauhaus_update_module] unsupported slider data type\n");
        }
        break;
      case DT_BAUHAUS_COMBOBOX:
        switch(bhw->field_type)
        {
          case DT_INTROSPECTION_TYPE_ENUM:
            dt_bauhaus_combobox_set_from_value(widget, *(int *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_INT:
            dt_bauhaus_combobox_set(widget, *(int *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_UINT:
            dt_bauhaus_combobox_set(widget, *(unsigned int *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_BOOL:
            dt_bauhaus_combobox_set(widget, *(gboolean *)bhw->field);
            break;
          default:
            fprintf(stderr, "[dt_bauhaus_update_module] unsupported combo data type\n");
        }
        break;
      default:
        fprintf(stderr, "[dt_bauhaus_update_module] invalid bauhaus widget type encountered\n");
    }
  }
}

// make this quad a toggle button:
void dt_bauhaus_widget_set_quad_toggle(GtkWidget *widget, int toggle)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_toggle = toggle;
}

void dt_bauhaus_widget_set_quad_active(GtkWidget *widget, int active)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if (active)
    w->quad_paint_flags |= CPF_ACTIVE;
  else
    w->quad_paint_flags &= ~CPF_ACTIVE;
  gtk_widget_queue_draw(GTK_WIDGET(w));
}

void dt_bauhaus_widget_set_quad_visibility(GtkWidget *widget, const gboolean visible)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->show_quad = visible;
  gtk_widget_queue_draw(GTK_WIDGET(w));
}

int dt_bauhaus_widget_get_quad_active(GtkWidget *widget)
{
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  return (w->quad_paint_flags & CPF_ACTIVE) == CPF_ACTIVE;
}

void dt_bauhaus_widget_press_quad(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if (w->quad_toggle)
  {
    w->quad_paint_flags ^= CPF_ACTIVE;
  }
  else
    w->quad_paint_flags |= CPF_ACTIVE;

  g_signal_emit_by_name(G_OBJECT(w), "quad-pressed");
}

void dt_bauhaus_widget_release_quad(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if (!w->quad_toggle)
  {
    if (w->quad_paint_flags & CPF_ACTIVE)
      w->quad_paint_flags &= ~CPF_ACTIVE;
    gtk_widget_queue_draw(GTK_WIDGET(w));
  }
}

GtkWidget *dt_bauhaus_slider_new(dt_iop_module_t *self)
{
  return dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.1, 0.5, 3);
}

GtkWidget *dt_bauhaus_slider_new_with_range(dt_iop_module_t *self, float min, float max, float step,
                                            float defval, int digits)
{
  return dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, step, defval, digits, 1);
}

GtkWidget *dt_bauhaus_slider_new_action(dt_action_t *self, float min, float max, float step,
                                        float defval, int digits)
{
  return dt_bauhaus_slider_new_with_range((dt_iop_module_t *)self, min, max, step, defval, digits);
}

GtkWidget *dt_bauhaus_slider_new_with_range_and_feedback(dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  return dt_bauhaus_slider_from_widget(w,self, min, max, step, defval, digits, feedback);
}

static void _style_updated(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = (struct dt_bauhaus_widget_t *)widget;
  _margins_retrieve(w);

  // gtk_widget_set_size_request is the minimal preferred, size.
  // it NEEDS to be defined and will be contextually adapted, possibly overriden by CSS.
  // Thing is Gtk CSS min-width in combination with hexpand is wonky so this is how it should be done.
  if(w->type == DT_BAUHAUS_COMBOBOX)
    gtk_widget_set_size_request(widget, DT_PIXEL_APPLY_DPI(50), _get_combobox_height(widget));
  else if(w->type == DT_BAUHAUS_SLIDER)
    gtk_widget_set_size_request(widget, DT_PIXEL_APPLY_DPI(180), _get_slider_height(widget));
}

GtkWidget *dt_bauhaus_slider_from_widget(dt_bauhaus_widget_t* w,dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback)
{
  w->type = DT_BAUHAUS_SLIDER;
  _bauhaus_widget_init(w, self);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->min = d->soft_min = d->hard_min = min;
  d->max = d->soft_max = d->hard_max = max;
  d->step = step;
  // normalize default:
  d->defpos = defval;
  d->pos = (defval - min) / (max - min);
  d->oldpos = d->pos;
  d->digits = digits;
  d->format = "";
  d->factor = 1.0f;
  d->offset = 0.0f;

  d->grad_cnt = 0;
  d->grad_col = NULL;
  d->grad_pos = NULL;

  d->fill_feedback = feedback;

  d->is_dragging = 0;
  d->timeout_handle = 0;

  dt_gui_add_class(GTK_WIDGET(w), "bauhaus_slider");

  g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(dt_bauhaus_slider_button_press), NULL);
  g_signal_connect(G_OBJECT(w), "button-release-event", G_CALLBACK(dt_bauhaus_slider_button_release), NULL);
  g_signal_connect(G_OBJECT(w), "motion-notify-event", G_CALLBACK(dt_bauhaus_slider_motion_notify), NULL);

  return GTK_WIDGET(w);
}

GtkWidget *dt_bauhaus_combobox_new(dt_iop_module_t *self)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  dt_bauhaus_combobox_from_widget(w,self);
  return GTK_WIDGET(w);
}

GtkWidget *dt_bauhaus_combobox_new_action(dt_action_t *self)
{
  return dt_bauhaus_combobox_new((dt_iop_module_t *)self);
}

GtkWidget *dt_bauhaus_combobox_new_full(dt_action_t *action, const char *section, const char *label, const char *tip,
                                        int pos, GtkCallback callback, gpointer data, const char **texts)
{
  GtkWidget *combo = dt_bauhaus_combobox_new_action(action);
  dt_bauhaus_widget_set_label(combo, section, label);
  dt_bauhaus_combobox_add_list(combo, (dt_action_t *)(DT_BAUHAUS_WIDGET(combo)->module), texts);
  dt_bauhaus_combobox_set(combo, pos);
  gtk_widget_set_tooltip_text(combo, tip ? tip : _(label));
  if(callback) g_signal_connect(G_OBJECT(combo), "value-changed", G_CALLBACK(callback), data);

  return combo;
}

void dt_bauhaus_combobox_from_widget(dt_bauhaus_widget_t* w,dt_iop_module_t *self)
{
  w->type = DT_BAUHAUS_COMBOBOX;
  _bauhaus_widget_init(w, self);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->entries = g_ptr_array_new_full(4, free_combobox_entry);
  d->defpos = 0;
  d->active = -1;
  d->editable = 0;
  d->text_align = DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT;
  d->entries_ellipsis = PANGO_ELLIPSIZE_END;
  d->populate = NULL;
  d->text = NULL;
  d->timeout_handle = 0;

  dt_gui_add_class(GTK_WIDGET(w), "bauhaus_combobox");

  g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(dt_bauhaus_combobox_button_press), NULL);
}

static dt_bauhaus_combobox_data_t *_combobox_data(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return NULL;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  if(d->active >= d->entries->len) d->active = -1;

  return d;
}

void dt_bauhaus_combobox_add_populate_fct(GtkWidget *widget, void (*fct)(GtkWidget *w, struct dt_iop_module_t **module))
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type == DT_BAUHAUS_COMBOBOX)
    w->data.combobox.populate = fct;
}

void dt_bauhaus_combobox_add_list(GtkWidget *widget, dt_action_t *action, const char **texts)
{
  if(action)
    g_hash_table_insert(darktable.control->combo_list, action, texts);

  while(texts && *texts)
    dt_bauhaus_combobox_add_full(widget, _(*(texts++)), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add(GtkWidget *widget, const char *text)
{
  dt_bauhaus_combobox_add_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add_aligned(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align)
{
  dt_bauhaus_combobox_add_full(widget, text, align, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                  gpointer data, void (free_func)(void *data), gboolean sensitive)
{
  if(darktable.control->accel_initialising) return;

  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  dt_bauhaus_combobox_entry_t *entry = new_combobox_entry(text, align, sensitive, data, free_func);
  g_ptr_array_add(d->entries, entry);
  if(d->active < 0) d->active = 0;
}

gboolean dt_bauhaus_combobox_set_entry_label(GtkWidget *widget, const int pos, const gchar *label)
{
  // change the text to show for the entry
  // note that this doesn't break shortcuts but their names in the shortcut panel will remain the initial one
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(!d || pos < 0 || pos >= d->entries->len) return FALSE;
  dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, pos);
  g_free(entry->label);
  entry->label = g_strdup(label);
  return TRUE;
}

void dt_bauhaus_combobox_set_entries_ellipsis(GtkWidget *widget, PangoEllipsizeMode ellipis)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->entries_ellipsis = ellipis;
}

PangoEllipsizeMode dt_bauhaus_combobox_get_entries_ellipsis(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->entries_ellipsis : PANGO_ELLIPSIZE_END;
}

void dt_bauhaus_combobox_set_editable(GtkWidget *widget, int editable)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->editable = editable ? 1 : 0;
  if(d->editable && !d->text)
    d->text = calloc(1, DT_BAUHAUS_COMBO_MAX_TEXT);
}

int dt_bauhaus_combobox_get_editable(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->editable : 0;
}

void dt_bauhaus_combobox_set_selected_text_align(GtkWidget *widget, const dt_bauhaus_combobox_alignment_t text_align)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->text_align = text_align;
}

void dt_bauhaus_combobox_remove_at(GtkWidget *widget, int pos)
{
  dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  if(!d || pos < 0 || pos >= d->entries->len) return;

  // move active position up if removing anything before it
  // or when removing last position that is currently active.
  // this also sets active to -1 when removing the last remaining entry in a combobox.
  if(d->active > pos || d->active == d->entries->len-1)
    d->active--;

  g_ptr_array_remove_index(d->entries, pos);
}

void dt_bauhaus_combobox_insert(GtkWidget *widget, const char *text,int pos)
{
  dt_bauhaus_combobox_insert_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, pos);
}

void dt_bauhaus_combobox_insert_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                     gpointer data, void (*free_func)(void *), int pos)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  g_ptr_array_insert(d->entries, pos, new_combobox_entry(text, align, TRUE, data, free_func));
  if(d->active < 0) d->active = 0;
}

int dt_bauhaus_combobox_length(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->entries->len : 0;
}

const char *dt_bauhaus_combobox_get_text(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d) return NULL;

  if(d->active < 0)
  {
    return d->editable ? d->text : NULL;
  }
  else
  {
    const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
    return entry->label;
  }
}

gpointer dt_bauhaus_combobox_get_data(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || d->active < 0) return NULL;

  const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
  return entry->data;
}

void dt_bauhaus_combobox_clear(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->active = -1;
  g_ptr_array_set_size(d->entries, 0);
}

const char *dt_bauhaus_combobox_get_entry(GtkWidget *widget, int pos)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || pos < 0 || pos >= d->entries->len) return NULL;

  const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, pos);
  return entry->label;
}

void dt_bauhaus_combobox_set_text(GtkWidget *widget, const char *text)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || !d->editable) return;

  g_strlcpy(d->text, text, DT_BAUHAUS_COMBO_MAX_TEXT);
}

static gint _delayed_combobox_commit(gpointer data)
{
  // Commit combobox value change to pipeline history, handling a safety timout
  // so incremental scrollings don't trigger a recompute at every scroll step.

  // API-wise, this is fucked up because IOPs are the GUI parents of Bauhaus widgets.
  // Parents can call children modules (for methods and data structure),
  // but children should be independent from parents, so the dependency tree is linear and vertical,
  // and each child is a self-enclosed and self-reliant module.

  // The following introduces a circular dependency of children from their parent, that
  // violates the modularity principle, and any change in parent scope will need to be propagated into the children scope,
  // which is unexpected and unreliable given the non-existence of a dev doc.

  // A mitigation would be to define `dt_iop_gui_changed()` as a callback in the parent,
  // then handle the following as a call to an opaque callback defined at implementation time.
  // This would advertise the existence of the callback in the parent scope
  // AND make the following immune to changes in parent.

  struct dt_bauhaus_widget_t *w = data;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->timeout_handle = 0;
  g_signal_emit_by_name(G_OBJECT(w), "value-changed");

  // TODO: make this a callback in module scope
  if(w->field && w->module) {
    switch(w->field_type)
    {
      case DT_INTROSPECTION_TYPE_ENUM:;
        if(d->active >= 0)
        {
          const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
          int *e = w->field, preve = *e; *e = GPOINTER_TO_INT(entry->data);
          if(*e != preve) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &preve);
        }
        break;
      case DT_INTROSPECTION_TYPE_INT:;
        int *i = w->field, previ = *i; *i = d->active;
        if(*i != previ) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &previ);
        break;
      case DT_INTROSPECTION_TYPE_UINT:;
        unsigned int *u = w->field, prevu = *u; *u = d->active;
        if(*u != prevu) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &prevu);
        break;
      case DT_INTROSPECTION_TYPE_BOOL:;
        gboolean *b = w->field, prevb = *b; *b = d->active;
        if(*b != prevb) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &prevb);
        break;
      default:
        fprintf(stderr, "[_bauhaus_combobox_set] unsupported combo data type\n");
    }
  }

  return G_SOURCE_REMOVE;
}

/**
 * @brief Set a combobox to a given integer position. Private API function, called from user events.
 *
 * @param widget
 * @param pos -1 for "custom" value in editable comboboxes, >= 0 for items in the list
 * @param timeout TRUE to apply an adaptative timeout preventing intermediate setting steps (e.g. while scrolling)
 * to emit too many value-changed signals and committing to pipeline. FALSE forces immediate dispatch of new value,
 * when there is no ambiguity that the setting is final (e.g left click).
 */
void _combobox_set(GtkWidget *widget, const int pos, gboolean timeout)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  const int old_pos = d->active;
  const int new_pos = (d->entries) ? CLAMP(pos, -1, (int)d->entries->len - 1)
                                   : -1;

  if(old_pos != new_pos)
  {
    d->active = new_pos;

#if DEBUG
    fprintf(stdout, "%s | %s | %s | item : %i \n",
    (w->module && w->module->owner) ? "has owner" : "orphan",
    (w->module) ? w->module->label : "anon",
    w->label,
    d->active);
#endif

    if(darktable.bauhaus->current == w)
      gtk_widget_queue_draw(darktable.bauhaus->popup_area);

    gtk_widget_queue_draw(GTK_WIDGET(w));

    if(!darktable.gui->reset)
    {
      if(d->timeout_handle)
      {
        g_source_remove(d->timeout_handle);
        d->timeout_handle = 0;
      }
      // TODO: map the timeout to an user config ? Arguably, that value will be higher for senior citizen
      d->timeout_handle = g_timeout_add(350, _delayed_combobox_commit, w);
    }
  }
}

// Public API function, called from GUI init and update
void dt_bauhaus_combobox_set(GtkWidget *widget, const int pos)
{
  _combobox_set(widget, pos, FALSE);
}


gboolean dt_bauhaus_combobox_set_from_text(GtkWidget *widget, const char *text)
{
  if(!text) return FALSE;

  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  for(int i = 0; d && i < d->entries->len; i++)
  {
    const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, i);
    if(!g_strcmp0(entry->label, text))
    {
      dt_bauhaus_combobox_set(widget, i);
      return TRUE;
    }
  }
  return FALSE;
}

gboolean dt_bauhaus_combobox_set_from_value(GtkWidget *widget, int value)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  for(int i = 0; d && i < d->entries->len; i++)
  {
    const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, i);
    if(GPOINTER_TO_INT(entry->data) == value)
    {
      dt_bauhaus_combobox_set(widget, i);
      return TRUE;
    }
  }
  return FALSE;
}

int dt_bauhaus_combobox_get(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->active : -1;
}

void dt_bauhaus_combobox_entry_set_sensitive(GtkWidget *widget, int pos, gboolean sensitive)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || pos < 0 || pos >= d->entries->len) return;

  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)g_ptr_array_index(d->entries, pos);
  entry->sensitive = sensitive;
}

void dt_bauhaus_slider_clear_stops(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->grad_cnt = 0;
}

void dt_bauhaus_slider_set_stop(GtkWidget *widget, float stop, float r, float g, float b)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  if(!d->grad_col)
  {
    d->grad_col = malloc(DT_BAUHAUS_SLIDER_MAX_STOPS * sizeof(*d->grad_col));
    d->grad_pos = malloc(DT_BAUHAUS_SLIDER_MAX_STOPS * sizeof(*d->grad_pos));
  }
  // need to replace stop?
  for(int k = 0; k < d->grad_cnt; k++)
  {
    if(d->grad_pos[k] == stop)
    {
      d->grad_col[k][0] = r;
      d->grad_col[k][1] = g;
      d->grad_col[k][2] = b;
      return;
    }
  }
  // new stop:
  if(d->grad_cnt < DT_BAUHAUS_SLIDER_MAX_STOPS)
  {
    int k = d->grad_cnt++;
    d->grad_pos[k] = stop;
    d->grad_col[k][0] = r;
    d->grad_col[k][1] = g;
    d->grad_col[k][2] = b;
  }
  else
  {
    fprintf(stderr, "[bauhaus_slider_set_stop] only %d stops allowed.\n", DT_BAUHAUS_SLIDER_MAX_STOPS);
  }
}

static void draw_equilateral_triangle(cairo_t *cr, float radius)
{
  cairo_arc(cr, 0, 0, radius, 0, M_PI * 2);
}

static void dt_bauhaus_draw_indicator(struct dt_bauhaus_widget_t *w, float pos, cairo_t *cr, float wd, const GdkRGBA fg_color, const GdkRGBA border_color)
{
  // draw scale indicator (the tiny triangle)
  const float size = darktable.bauhaus->marker_size;
  const float vertical_position = _get_indicator_y_position();
  const float horizontal_position = pos * wd;

  cairo_save(cr);

  // Clip horizontally to the limit of the baseline background
  cairo_rectangle(cr, 0, 0, wd, 200);
  cairo_clip(cr);
  cairo_translate(cr, horizontal_position, vertical_position);

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  if(d->fill_feedback)
  {
    // Plain indicator (regular sliders)
    draw_equilateral_triangle(cr, size / 2.);
    cairo_set_line_width(cr, 0);
    cairo_fill(cr);
  }
  else
  {
    // Hollow indicator to see a color through it (gradient sliders)
    const float border = (size - darktable.bauhaus->baseline_size) / 2.;
    cairo_set_line_width(cr, border);
    draw_equilateral_triangle(cr, size / 2. - border / 2.);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

static void dt_bauhaus_draw_quad(struct dt_bauhaus_widget_t *w, cairo_t *cr, const double x, const double y)
{
  if(!w->show_quad) return;

  cairo_save(cr);
  if(w->quad_paint)
  {
    // draw color picker
    w->quad_paint(cr, x, y,
                      darktable.bauhaus->quad_width,          // width
                      darktable.bauhaus->quad_width,          // height
                      w->quad_paint_flags, w->quad_paint_data);
  }
  else if(w->type == DT_BAUHAUS_COMBOBOX)
  {
    // draw combobox chevron
    cairo_translate(cr, x + darktable.bauhaus->quad_width / 2., y + darktable.bauhaus->quad_width / 2.);
    const float r = darktable.bauhaus->quad_width * .2f;
    cairo_move_to(cr, -r, -r * .5f);
    cairo_line_to(cr, 0, r * .5f);
    cairo_line_to(cr, r, -r * .5f);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

/**
 * @brief Draw the slider baseline, aka the backgronud bar.
 *
 * @param w Widget
 * @param cr Cairo object
 * @param width The width of the actual slider baseline (corrected for padding, margin and quad width if needed)
 */
static void dt_bauhaus_draw_baseline(struct dt_bauhaus_widget_t *w, cairo_t *cr, float width)
{
  // draw line for orientation in slider
  cairo_save(cr);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  const float baseline_top = darktable.bauhaus->line_height + INNER_PADDING;
  const float baseline_height = darktable.bauhaus->baseline_size;

  // the background of the line
  cairo_rectangle(cr, 0, baseline_top, width, baseline_height);
  cairo_pattern_t *gradient = NULL;
  if(d->grad_cnt > 0)
  {
    // gradient line as used in some modules for hue, saturation, lightness
    const double zoom = (d->max - d->min) / (d->hard_max - d->hard_min);
    const double offset = (d->min - d->hard_min) / (d->hard_max - d->hard_min);
    gradient = cairo_pattern_create_linear(0, 0, width, baseline_height);
    for(int k = 0; k < d->grad_cnt; k++)
      cairo_pattern_add_color_stop_rgba(gradient, (d->grad_pos[k] - offset) / zoom,
                                        d->grad_col[k][0], d->grad_col[k][1], d->grad_col[k][2], 0.4f);
    cairo_set_source(cr, gradient);
  }
  else
  {
    // regular baseline
    set_color(cr, darktable.bauhaus->color_bg);
  }
  cairo_fill(cr);
  if(gradient) cairo_pattern_destroy(gradient);

  // get the reference of the slider aka the position of the 0 value
  const float origin = fmaxf(fminf((d->factor > 0 ? -d->min - d->offset/d->factor
                                                  :  d->max + d->offset/d->factor)
                                                  / (d->max - d->min), 1.0f) * width, 0.0f);
  const float position = d->pos * width;
  const float delta = position - origin;

  // have a `fill ratio feel' from zero to current position
  if(d->fill_feedback)
  {
    // only brighten, useful for colored sliders to not get too faint:
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SCREEN);
    set_color(cr, darktable.bauhaus->color_fill);
    cairo_rectangle(cr, origin, baseline_top, delta, baseline_height);
    cairo_fill(cr);
    cairo_restore(cr);
  }

  // draw the 0 reference graduation if it's different than the bounds of the slider
  const float graduation_top = baseline_top + darktable.bauhaus->marker_size + darktable.bauhaus->border_width;
  set_color(cr, darktable.bauhaus->color_fg);

  // If the max of the slider is 360, it is likely an absolute hue slider in degrees
  // a zero in periodic stuff has not much meaning so we skip it.
  if(d->hard_max != 360.0f)
  {
    cairo_arc(cr, origin, graduation_top, darktable.bauhaus->border_width / 2., 0, 2 * M_PI);
    cairo_fill(cr);
  }

  cairo_restore(cr);
}

static void dt_bauhaus_widget_reject(struct dt_bauhaus_widget_t *w)
{
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      break;
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      dt_bauhaus_slider_set_normalized(w, d->oldpos, TRUE, FALSE);
    }
    break;
    default:
      break;
  }
}

static void dt_bauhaus_widget_accept(struct dt_bauhaus_widget_t *w, gboolean timeout)
{
  GtkWidget *widget = GTK_WIDGET(w);

  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      dt_bauhaus_combobox_data_t *d = &w->data.combobox;

      if(d->editable && darktable.bauhaus->keys_cnt > 0)
      {
        // combobox is editable and we have text, assume it is a custom input
        memset(d->text, 0, DT_BAUHAUS_COMBO_MAX_TEXT);
        g_strlcpy(d->text, darktable.bauhaus->keys, DT_BAUHAUS_COMBO_MAX_TEXT);
        _combobox_set(widget, -1, timeout); // select custom entry

#if DEBUG
        fprintf(stdout, "combobox went the custom path\n");
#endif
      }
      else
      {
        if(darktable.bauhaus->keys_cnt > 0)
        {
          // combobox is not editable, but we have text. Assume user wanted to init a selection from keyboard.
          // find the closest match by looking for the entry having the maximum number
          // of characters in common with the user input.
          gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, -1);
          int match = -1;
          int matches = 0;

          for(int j = 0; j < d->entries->len; j++)
          {
            const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, j);
            gchar *text_cmp = g_utf8_casefold(entry->label, -1);
            if(entry->sensitive && !strncmp(text_cmp, keys, darktable.bauhaus->keys_cnt))
            {
              matches++;
              match = j;
            }
            g_free(text_cmp);
          }
          g_free(keys);

          // Accept result only if exactly one match was found. Anything else is ambiguous
          if(matches == 1)
            _combobox_set(widget, match, timeout);
        }
        else {
          // Active entry (below cursor or scrolled)
          _combobox_set(widget, d->hovered, timeout);
        }
      }

      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      // The slider popup uses the quadratic magnifier for accurate setting.
      // We need extra conversions from cursor coordinates to set it right.
      // This needs to be kept in sync with popup_draw()
      dt_bauhaus_slider_data_t *d = &w->data.slider;

      // This is needed to accept the change.
      // d->pos is soft-updated with corrected coordinates for drawing purposes only in popup_redraw().
      // We need to reset it to the original value temporarily, and request a proper setting
      // with value-changed signal through dt_bauhaus_slider_set_normalized()
      const float value = d->pos;
      d->pos = d->oldpos;
      dt_bauhaus_slider_set_normalized(w, value, TRUE, timeout);
      break;
    }
    default:
      break;
  }
}

static gchar *_build_label(const struct dt_bauhaus_widget_t *w)
{
  if(w->show_extended_label && w->section)
    return g_strdup_printf("%s - %s", w->section, w->label);
  else
    return g_strdup(w->label);
}

static gboolean dt_bauhaus_popup_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  // Popups belong to the app, not to the bauhaus widget. That's confusing.
  // Also the *widget param here is the popup container, still not the bauhaus widget. Even more confusing.
  // This is the actual parent bauhaus widget :
  struct dt_bauhaus_widget_t *w = darktable.bauhaus->current;

  // get area properties
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(cst);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  // look up some colors once
  GdkRGBA text_color, text_color_selected, text_color_hover, text_color_insensitive, text_color_focused;
  gtk_style_context_get_color(context, GTK_STATE_FLAG_NORMAL, &text_color);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_SELECTED, &text_color_selected);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_PRELIGHT, &text_color_hover);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_INSENSITIVE, &text_color_insensitive);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_FOCUSED, &text_color_focused);

  GdkRGBA *fg_color = default_color_assign();
  GdkRGBA *bg_color;
  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  gtk_style_context_get(context, state, "background-color", &bg_color, NULL);
  gtk_style_context_get_color(context, state, fg_color);

  // draw background
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);
  gtk_render_frame(context, cr, 0, 0, allocation.width, allocation.height);

  const double main_height = _widget_get_main_height(w, widget);
  double total_width = 0.f;
  const double main_width = _widget_get_main_width(w, NULL, &total_width);

  // translate to account for the widget spacing
  cairo_translate(cr, w->padding->left, w->padding->top);

  // switch on bauhaus widget type (so we only need one static window)
  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      cairo_save(cr);
      set_color(cr, *fg_color);

      float scale = _bh_slider_get_scale(w);
      const int num_scales = 1.f / scale;
      const float bottom_baseline = _get_slider_bar_height();

      for(int k = 0; k < num_scales; k++)
      {
        const float off = k * scale - d->oldpos;
        GdkRGBA fg_copy = *fg_color;
        fg_copy.alpha = scale / fabsf(off);
        set_color(cr, fg_copy);
        draw_slider_line(cr, d->oldpos, off, scale, main_width, main_height, bottom_baseline, 1);
        cairo_stroke(cr);
      }
      cairo_restore(cr);

      // Get the x offset compared to d->oldpos accounting for vertical position magnification
      const double mouse_off = d->pos - d->oldpos;

      // Draw the baseline with fill feedback if any (needs the new d->pos set before)
      dt_bauhaus_draw_baseline(w, cr, main_width);

      cairo_save(cr);

      // draw mouse over indicator line
      set_color(cr, *fg_color);
      draw_slider_line(cr, d->oldpos, mouse_off, scale, main_width, main_height, bottom_baseline, 2);
      cairo_stroke(cr);

      // draw indicator
      dt_bauhaus_draw_indicator(w, d->pos, cr, main_width, *fg_color, *bg_color);

      cairo_restore(cr);

      // draw numerical value:
      cairo_save(cr);
      if(state & GTK_STATE_FLAG_PRELIGHT)
        set_color(cr, text_color_hover);
      else if(state & GTK_STATE_FLAG_FOCUSED)
        set_color(cr, text_color_focused);
      else if(state & GTK_STATE_FLAG_INSENSITIVE)
        set_color(cr, text_color_insensitive);
      else
        set_color(cr, *fg_color);

      float value_width = 0.f;
      char *text = dt_bauhaus_slider_get_text(GTK_WIDGET(w), dt_bauhaus_slider_get(GTK_WIDGET(w)));
      GdkRectangle bounding_value = { .x = 0.,
                                      .y = 0.,
                                      .width = main_width,
                                      .height = darktable.bauhaus->line_height };
      // Display user keyboard input if any, otherwise the current value
      show_pango_text(w, context, cr, &bounding_value,
                      (darktable.bauhaus->keys_cnt) ? darktable.bauhaus->keys : text, BH_ALIGN_RIGHT,
                      BH_ALIGN_MIDDLE, PANGO_ELLIPSIZE_NONE, NULL, &value_width, NULL, FALSE);
      g_free(text);

      // label on top of marker:
      gchar *label_text = _build_label(w);
      const float label_width = main_width - value_width - INNER_PADDING;
      GdkRectangle bounding_label = { .x = 0.,
                                      .y = 0.,
                                      .width = label_width,
                                      .height = darktable.bauhaus->line_height };
      show_pango_text(w, context, cr, &bounding_label, label_text, BH_ALIGN_LEFT, BH_ALIGN_MIDDLE,
                      PANGO_ELLIPSIZE_END, NULL, NULL, NULL, FALSE);
      g_free(label_text);

      cairo_restore(cr);
    }
    break;
    case DT_BAUHAUS_COMBOBOX:
    {
      float cumulative_height = 0.f;
      const dt_bauhaus_combobox_data_t *d = &w->data.combobox;

      // User keyboard input goes first
      if(darktable.bauhaus->keys_cnt > 0)
      {
        cairo_save(cr);
        set_color(cr, text_color_focused);
        GdkRectangle query_label = { .x = 0.,
                                     .y = cumulative_height,
                                     .width = main_width,
                                     .height = _bh_get_row_height() };
        show_pango_text(w, context, cr, &query_label, darktable.bauhaus->keys, BH_ALIGN_RIGHT, BH_ALIGN_MIDDLE,
                        PANGO_ELLIPSIZE_NONE, NULL, NULL, NULL, FALSE);
        cumulative_height += query_label.height;
        cairo_restore(cr);
      }

      cairo_save(cr);
      gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, -1);
      for(int j = 0; j < d->entries->len; j++)
      {
        const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, j);
        gchar *text_cmp = g_utf8_casefold(entry->label, -1);
        if(!strncmp(text_cmp, keys, darktable.bauhaus->keys_cnt))
        {
          // If user typed some keys, display matching entries only
          gboolean ignore_font_style = FALSE;
          if(!entry->sensitive)
            set_color(cr, text_color_insensitive);
          else if(j == d->hovered)
            set_color(cr, text_color_hover);
          else if(j == d->active)
            set_color(cr, text_color_selected);
          else
          {
            set_color(cr, text_color);
            // GTK_STATE flag is applied to the whole widget.
            // We need to selectively dispatch here to the right internal elements.
            ignore_font_style = TRUE;
          }

          GdkRectangle bounding_label = { .x = 0.,
                                          .y = cumulative_height,
                                          .width = main_width,
                                          .height = _bh_get_row_height() };
          show_pango_text(w, context, cr, &bounding_label, entry->label, BH_ALIGN_RIGHT, BH_ALIGN_TOP,
                          d->entries_ellipsis, bg_color, NULL, NULL, ignore_font_style);
          cumulative_height += bounding_label.height;
        }

        g_free(text_cmp);
      }
      cairo_restore(cr);
      g_free(keys);
    }
    break;
    default:
      // yell
      break;
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  gdk_rgba_free(bg_color);
  gdk_rgba_free(fg_color);

  return TRUE;
}

static gboolean _widget_draw(GtkWidget *widget, cairo_t *crf)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(cst);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  GdkRGBA *bg_color = default_color_assign();
  GdkRGBA *text_color = default_color_assign();
  const GtkStateFlags state = gtk_widget_get_state_flags(widget);
  gtk_style_context_get_color(context, state, text_color);
  gtk_style_context_get(context, state, "background-color", &bg_color, NULL);
  _margins_retrieve(w);

  // Paint background first
  gtk_render_background(context, cr, allocation.x, allocation.y, allocation.width, allocation.height);

  // Translate Cairo coordinates to account for the widget spacing
  const float available_width = _widget_get_main_width(w, NULL, NULL);
  const float inner_height = _widget_get_main_height(w, NULL);
  cairo_translate(cr, w->margin->left + w->padding->left, w->margin->top + w->padding->top);

  // draw type specific content:
  cairo_save(cr);
  set_color(cr, *text_color);
  cairo_set_line_width(cr, 1.0);
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      // draw label and quad area at right end
      if(w->show_quad)
        dt_bauhaus_draw_quad(w, cr, available_width + 2. * INNER_PADDING, 0.);

      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      const PangoEllipsizeMode combo_ellipsis = d->entries_ellipsis;
      const gchar *text = d->text;
      if(d->active >= 0 && d->active < d->entries->len)
      {
        const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
        text = entry->label;
      }

      gchar *label_text = _build_label(w);
      float label_width = 0.f;
      float label_height = 0.f;

      GdkRectangle bounding_label = { .x = 0.,
                                      .y = 0.,
                                      .width = available_width,
                                      .height = inner_height };
      if(label_text && w->show_label)
        show_pango_text(w, context, cr, &bounding_label, label_text, BH_ALIGN_LEFT, BH_ALIGN_MIDDLE,
                        combo_ellipsis, NULL, &label_width, &label_height, FALSE);

      // The value is shown right-aligned, ellipsized if needed.
      GdkRectangle bounding_value = { .x = label_width + INNER_PADDING,
                                      .y = 0.,
                                      .width = available_width - label_width - INNER_PADDING,
                                      .height = inner_height };
      show_pango_text(w, context, cr, &bounding_value, text, BH_ALIGN_RIGHT, BH_ALIGN_MIDDLE, combo_ellipsis, NULL,
                      NULL, NULL, FALSE);

      g_free(label_text);
      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      // line for orientation
      dt_bauhaus_draw_baseline(w, cr, available_width);

      // Paint the non-active quad icon with some transparency, because
      // icons are bolder than the neighbouring text and appear brighter.
      cairo_save(cr);
      if(!(w->quad_paint_flags & CPF_ACTIVE))
        cairo_set_source_rgba(cr, text_color->red, text_color->green, text_color->blue, text_color->alpha * 0.7);
      dt_bauhaus_draw_quad(w, cr, available_width + 2. * INNER_PADDING, 0.);
      cairo_restore(cr);

      float value_width = 0;
      if(gtk_widget_is_sensitive(widget))
      {
        cairo_save(cr);
        dt_bauhaus_draw_indicator(w, w->data.slider.pos, cr, available_width, *text_color, *bg_color);
        cairo_restore(cr);

        char *text = dt_bauhaus_slider_get_text(widget, dt_bauhaus_slider_get(widget));
        GdkRectangle bounding_value = { .x = 0.,
                                        .y = 0.,
                                        .width = available_width,
                                        .height = darktable.bauhaus->line_height };
        show_pango_text(w, context, cr, &bounding_value, text, BH_ALIGN_RIGHT, BH_ALIGN_MIDDLE,
                        PANGO_ELLIPSIZE_NONE, NULL, &value_width, NULL, FALSE);
        g_free(text);
      }

      // label on top of marker:
      gchar *label_text = _build_label(w);
      const float label_width = available_width - value_width - INNER_PADDING;
      GdkRectangle bounding_label = { .x = 0.,
                                      .y = 0.,
                                      .width = label_width,
                                      .height = darktable.bauhaus->line_height };
      show_pango_text(w, context, cr, &bounding_label, label_text, BH_ALIGN_LEFT, BH_ALIGN_MIDDLE,
                        PANGO_ELLIPSIZE_END, NULL, NULL, NULL, FALSE);
      g_free(label_text);
    }
    break;
    default:
      break;
  }
  cairo_restore(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  gdk_rgba_free(text_color);
  gdk_rgba_free(bg_color);

  return TRUE;
}

static void _get_preferred_width(GtkWidget *widget, gint *minimum_size, gint *natural_size)
{
  // Nothing clever here : preferred size is the size of the container.
  // If user is not happy with that, it's his responsibility to resize sidebars.
  if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_RIGHT, widget))
    *natural_size = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_RIGHT);
  else if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_LEFT, widget))
    *natural_size = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_LEFT);
  else
    *natural_size = DT_PIXEL_APPLY_DPI(300);
}

void dt_bauhaus_hide_popup()
{
  if(darktable.bauhaus->current)
  {
    gtk_grab_remove(darktable.bauhaus->popup_area);
    gtk_widget_hide(darktable.bauhaus->popup_window);
    gtk_window_set_attached_to(GTK_WINDOW(darktable.bauhaus->popup_window), NULL);
    darktable.bauhaus->current = NULL;
  }
}

void dt_bauhaus_show_popup(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(darktable.bauhaus->current) dt_bauhaus_hide_popup();
  darktable.bauhaus->current = w;
  darktable.bauhaus->keys_cnt = 0;
  memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
  darktable.bauhaus->change_active = 0;
  darktable.bauhaus->mouse_line_distance = 0.0f;
  darktable.bauhaus->hiding = FALSE;

  // Make sure all relevant widgets exist
  gtk_widget_realize(darktable.bauhaus->popup_window);
  gtk_widget_realize(widget);

  GtkAllocation tmp;
  gtk_widget_get_allocation(widget, &tmp);
  int width = tmp.width;
  int height = tmp.height;

  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      // Slider popup: make it square
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->oldpos = d->pos;
      d->is_dragging = FALSE;
      height = tmp.width;
      break;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      height = floorf(_get_combobox_popup_height(w));
      break;
    }
    default:
    {
      fprintf(stderr, "[dt_bauhaus_show_popup] The bauhaus widget has an unknown type\n");
      break;
    }
  }

  /* Bind to CSS rules from parent widget */
  GtkStyleContext *context = gtk_widget_get_style_context(darktable.bauhaus->popup_area);
  gtk_style_context_add_class(context, "dt_bauhaus_popup");
  gtk_window_set_attached_to(GTK_WINDOW(darktable.bauhaus->popup_window), widget);

  // Get the origin coordinates of the parent widget allocation box with regard to the main window
  gint wx = 0, wy = 0;
  GdkWindow *widget_window = gtk_widget_get_window(widget);
  if(widget_window) gdk_window_get_origin(widget_window, &wx, &wy);

#if GTK_CHECK_VERSION(3, 24, 0)
  // Get the origin coordinates of the main window box with regard to the screen
  gint wwx = 0, wwy = 0;
  gdk_window_get_origin(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)), &wwx, &wwy);

  // Final coordinates of the allocation box where to anchor the popup
  tmp.x = wx - wwx;
  tmp.y = wy - wwy;
#else
  // From here, all positions are float so we need to handle them as such
  // to avoid rounding errors resulting is pixel-shifting glitches.
  float x = (float)wx;
  float y = (float)wy;

  // The popup is a drop-down, meaning we can be sure it will not go higher than the calling widget.
  // But it can still exit the viewport by the bottom.
  // When that's the case, shift it vertically until it fits, assuming
  // the combobox is smaller than the viewport.
  // If that's not enough, then reduce comboboxes items or use a scrolled window.
  const int min_y = floorf(y) + height;
  if(darktable.gui->ui->manager.viewport.height < min_y)
    y = MAX(0, y - (min_y - darktable.gui->ui->manager.viewport.height));
#endif

  // Set desired size, but it's more a guide than a rule.
  gtk_widget_set_size_request(darktable.bauhaus->popup_window, width, height);

  // Need to call resize to actually change something
  gtk_window_resize(GTK_WINDOW(darktable.bauhaus->popup_window), width, height);

  GdkWindow *window = gtk_widget_get_window(darktable.bauhaus->popup_window);

#if GTK_CHECK_VERSION(3, 24, 0)
  // For Wayland (and supposed to work on X11 too) and Gtk 3.24 this is how you do it

  // move_to_rect below needs visible widgets to compute sizing properly.
  // That makes the GUI glitch but I have no better solution…
  gtk_widget_show(darktable.bauhaus->popup_window);

  gdk_window_move_to_rect(GDK_WINDOW(window), &tmp, GDK_GRAVITY_NORTH, GDK_GRAVITY_NORTH,
                          GDK_ANCHOR_SLIDE_Y, 0, 0);
#else
  // X11 wonky way of setting window position in absolute coordinates
  gdk_window_move(window, (gint)x, (gint)y);
#endif

  gtk_widget_show_all(darktable.bauhaus->popup_window);
  gtk_widget_grab_focus(darktable.bauhaus->popup_area);
}

static void _slider_add_step(GtkWidget *widget, float delta, guint state, gboolean force)
{
  if(delta == 0) return;
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  delta *= dt_bauhaus_slider_get_step(widget) * dt_accel_get_speed_multiplier(widget, state);

  const float min_visible = 1.f / (fabsf(d->factor) * ipow(10, d->digits));
  if(delta && fabsf(delta) < min_visible)
    delta = copysignf(min_visible, delta);

  const float value = dt_bauhaus_slider_get(widget);

  if(force || dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
  {
    if(d->factor > 0 ? d->pos < 0.0001 : d->pos > 0.9999) d->min = d->soft_min;
    if(d->factor < 0 ? d->pos < 0.0001 : d->pos > 0.9999) d->max = d->soft_max;
    dt_bauhaus_slider_set(widget, value + delta);
  }
  else
    dt_bauhaus_slider_set(widget, CLAMP(value + delta, d->min, d->max));
}

static gboolean _widget_scroll(GtkWidget *widget, GdkEventScroll *event)
{
  if(darktable.gui->has_scroll_focus != widget) return FALSE;

  // handle speed adjustment in mapping mode in dispatcher
  if(darktable.control->mapping_widget)
    return dt_shortcut_dispatcher(widget, (GdkEvent*)event, NULL);

  int delta_y = 0;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(delta_y == 0) return TRUE;
    struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

    if(w->type == DT_BAUHAUS_SLIDER)
    {
      gboolean force = darktable.control->element == DT_ACTION_ELEMENT_FORCE;
      if(force && dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
      {
        _slider_zoom_range(w, delta_y);
        _slider_zoom_toast(w);
      }
      else
        _slider_add_step(widget, - delta_y, event->state, force);
    }
    else
      _combobox_next_sensitive(w, delta_y);
  }
  return TRUE; // Ensure that scrolling the combobox cannot move side panel
}

static gboolean _widget_key_press(GtkWidget *widget, GdkEventKey *event)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  int delta = -1;
  switch(event->keyval)
  {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      delta = 1;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      bauhaus_request_focus(w);

      if(w->type == DT_BAUHAUS_SLIDER)
        _slider_add_step(widget, delta, event->state, FALSE);
      else
        _combobox_next_sensitive(w, -delta);

      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  struct dt_bauhaus_widget_t *w = (struct dt_bauhaus_widget_t *)widget;
  bauhaus_request_focus(w);

  double event_x = event->x;
  double event_y = event->y;
  double width;
  _bh_active_region_t activated = _bh_get_active_region(widget, &event_x, &event_y, &width, NULL);

  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(d->timeout_handle) g_source_remove(d->timeout_handle);
  d->timeout_handle = 0;

  if(activated == BH_REGION_OUT) return FALSE;

  if(activated == BH_REGION_QUAD && w->quad_toggle)
  {
    dt_bauhaus_widget_press_quad(widget);
    return TRUE;
  }
  else
  {
    // If no quad toggle, treat the whole widget as unit pack.
    if(event->button == 3)
    {
      darktable.bauhaus->mouse_x = event_x;
      darktable.bauhaus->mouse_y = event_y;
      dt_bauhaus_show_popup(widget);
      return TRUE;
    }
    else if(event->button == 1)
    {
      // reset to default.
      if(event->type == GDK_2BUTTON_PRESS)
      {
        // never called, as we popup the other window under your cursor before.
        // (except in weird corner cases where the popup is under the -1st entry
        _combobox_set(widget, d->defpos, FALSE);
        dt_bauhaus_hide_popup();
      }
      else
      {
        // single click, show options
        darktable.bauhaus->opentime = event->time;
        darktable.bauhaus->mouse_x = event_x;
        darktable.bauhaus->mouse_y = event_y;
        dt_bauhaus_show_popup(widget);
      }
      return TRUE;
    }
  }
  return FALSE;
}

float dt_bauhaus_slider_get(GtkWidget *widget)
{
  // first cast to bh widget, to check that type:
  const struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->min + d->pos * (d->max - d->min);
}

float dt_bauhaus_slider_get_val(GtkWidget *widget)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(widget)->data.slider;
  return dt_bauhaus_slider_get(widget) * d->factor + d->offset;
}

char *dt_bauhaus_slider_get_text(GtkWidget *w, float val)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(w)->data.slider;
  if((d->hard_max * d->factor + d->offset)*(d->hard_min * d->factor + d->offset) < 0)
    return g_strdup_printf("%+.*f%s", d->digits, val * d->factor + d->offset, d->format);
  else
    return g_strdup_printf( "%.*f%s", d->digits, val * d->factor + d->offset, d->format);
}

void dt_bauhaus_slider_set(GtkWidget *widget, float pos)
{
  // this is the public interface function, translate by bounds and call set_normalized
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  const float rpos = CLAMP(pos, d->hard_min, d->hard_max);
  d->min = MIN(d->min, rpos);
  d->max = MAX(d->max, rpos);
  dt_bauhaus_slider_set_normalized(w, (rpos - d->min) / (d->max - d->min), TRUE, FALSE);
}

void dt_bauhaus_slider_set_val(GtkWidget *widget, float val)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(widget)->data.slider;
  dt_bauhaus_slider_set(widget, (val - d->offset) / d->factor);
}

void dt_bauhaus_slider_set_digits(GtkWidget *widget, int val)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->digits = val;
}

int dt_bauhaus_slider_get_digits(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->digits;
}

void dt_bauhaus_slider_set_step(GtkWidget *widget, float val)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->step = val;
}

float dt_bauhaus_slider_get_step(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  float step = d->step;

  if(step == 0.f)
  {
    gboolean zoom = dt_conf_get_bool("bauhaus/zoom_step");
    const float min = zoom ? d->min : d->soft_min;
    const float max = zoom ? d->max : d->soft_max;

    const float top = fminf(max-min, fmaxf(fabsf(min), fabsf(max)));
    if(top >= 100.f)
    {
      step = 1.f;
    }
    else
    {
      step = top * fabsf(d->factor) / 100.f;
      const float log10step = log10f(step);
      const float fdigits = floorf(log10step + .1f);

      // using ipow here makes the UI hang indefinitly.
      // Why ? We have +/- inf in fdigits ?
      step = powf(10.f, fdigits);
      if(log10step - fdigits > .5f)
        step *= 5.f;
      step /= fabsf(d->factor);
    }
  }

  return copysignf(step, d->factor);
}

void dt_bauhaus_slider_set_feedback(GtkWidget *widget, int feedback)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->fill_feedback = feedback;
  gtk_widget_queue_draw(widget);
}

int dt_bauhaus_slider_get_feedback(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->fill_feedback;
}

void dt_bauhaus_slider_reset(GtkWidget *widget)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->min = d->soft_min;
  d->max = d->soft_max;
  dt_bauhaus_slider_set(widget, d->defpos);
  return;
}

void dt_bauhaus_slider_set_format(GtkWidget *widget, const char *format)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->format = g_intern_string(format);

  if(strstr(format,"%") && fabsf(d->hard_max) <= 10)
  {
    if(d->factor == 1.0f) d->factor = 100;
    d->digits -= 2;
  }
}

void dt_bauhaus_slider_set_factor(GtkWidget *widget, float factor)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->factor = factor;
}

void dt_bauhaus_slider_set_offset(GtkWidget *widget, float offset)
{
  struct dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->offset = offset;
}

static gboolean _delayed_slider_commit(gpointer data)
{
  // Commit slider value change to pipeline history, handling a safety timout
  // so incremental scrolls don't trigger a recompute at every scroll step.

  // API-wise, this is fucked up because IOPs are the GUI parents of Bauhaus widgets.
  // Parents can call children modules (for methods and data structure),
  // but children should be independent from parents, so the dependency tree is linear and vertical,
  // and each child is a self-enclosed and self-reliant module.

  // The following introduces a circular dependency of children from their parent, that
  // violates the modularity principle, and any change in parent scope will need to be propagated into the children scope,
  // which is unexpected and unreliable given the non-existence of a dev doc.

  // A mitigation would be to define `dt_iop_gui_changed()` as a callback in the parent,
  // then handle the following as a call to an opaque callback defined at implementation time.
  // This would advertise the existence of the callback in the parent scope
  // AND make the following immune to changes in parent.

  struct dt_bauhaus_widget_t *w = data;
  w->data.slider.timeout_handle = 0;
  g_signal_emit_by_name(G_OBJECT(w), "value-changed");

  // TODO: rewrite that as callback.
  if(w->field && w->module)
  {
    float val = dt_bauhaus_slider_get(GTK_WIDGET(w));
    switch(w->field_type)
    {
      case DT_INTROSPECTION_TYPE_FLOAT:;
        float *f = w->field, prevf = *f; *f = val;
        if(*f != prevf) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &prevf);
        break;
      case DT_INTROSPECTION_TYPE_INT:;
        int *i = w->field, previ = *i; *i = val;
        if(*i != previ) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &previ);
        break;
      case DT_INTROSPECTION_TYPE_USHORT:;
        unsigned short *s = w->field, prevs = *s; *s = val;
        if(*s != prevs) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &prevs);
        break;
      default:
        fprintf(stderr, "[_bauhaus_slider_value_change] unsupported slider data type\n");
    }
  }

  return G_SOURCE_REMOVE;
}

/**
 * @brief Set the value of a slider as a ratio of the GUI slider width
 *
 * @param w Bauhaus widget
 * @param pos Relative position over the slider bar (ratio between 0 and 1)
 * @param raise Set to FALSE to redraw slider position without committing the actual value to pipeline
 * nor sending the `value-changed` event (e.g. in motion-notify events, while dragging).
 * Set to TRUE when the change is finished (e.g. in button-pressed events).
 * @param timeout TRUE to add a timeout preventing intermediate setting steps (e.g. while scrolling) to emit
 * value-changed signal and commit to pipeline too often. FALSE to set immediately, when there is no ambiguity
 * on the final setting (e.g. at init time and on click). Doesn't change anything if raise is FALSE.
 */
static void dt_bauhaus_slider_set_normalized(struct dt_bauhaus_widget_t *w, float pos, gboolean raise, gboolean timeout)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  const float old_pos = d->pos;
  const float new_pos = CLAMP(pos, 0.0f, 1.0f);

  if(old_pos != new_pos || raise)
  {
    const float new_value = new_pos * (d->max - d->min) + d->min;
    const float precision = (float)ipow(10, d->digits) * d->factor;
    const float rounded_value = roundf(new_value * precision) / precision;
    d->pos = (rounded_value - d->min) / (d->max - d->min);

    if(darktable.bauhaus->current == w)
      gtk_widget_queue_draw(darktable.bauhaus->popup_area);

    gtk_widget_queue_draw(GTK_WIDGET(w));

#if DEBUG
    fprintf(stdout, "%s | %s | %s | min: %f, max: %f, ratio: %f, base : %f, pos: %f\n",
    (w->module && w->module->owner) ? "has owner" : "orphan",
    (w->module) ? w->module->label : "anon",
    w->label,
    d->min, d->max, d->pos, d->factor, d->pos);
#endif

    if(!darktable.gui->reset && raise)
    {
      if(d->timeout_handle)
      {
        g_source_remove(d->timeout_handle);
        d->timeout_handle = 0;
      }
      // TODO: map the timeout to an user config ? Arguably, that value will be higher for senior citizen
      d->timeout_handle = g_timeout_add(350, _delayed_slider_commit, w);
    }
  }
}

static gboolean dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      if(darktable.bauhaus->keys_cnt + 2 < 64
         && (event->keyval == GDK_KEY_space || event->keyval == GDK_KEY_KP_Space ||              // SPACE
             event->keyval == GDK_KEY_percent ||                                                 // %
             (event->string[0] >= 40 && event->string[0] <= 57) ||                               // ()+-*/.,0-9
             event->keyval == GDK_KEY_asciicircum || event->keyval == GDK_KEY_dead_circumflex || // ^
             event->keyval == GDK_KEY_X || event->keyval == GDK_KEY_x))                          // Xx
      {
        if(event->keyval == GDK_KEY_dead_circumflex)
          darktable.bauhaus->keys[darktable.bauhaus->keys_cnt++] = '^';
        else
          darktable.bauhaus->keys[darktable.bauhaus->keys_cnt++] = event->string[0];
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0
              && (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete))
      {
        darktable.bauhaus->keys[--darktable.bauhaus->keys_cnt] = 0;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0 && darktable.bauhaus->keys_cnt + 1 < 64
              && (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter))
      {
        // accept input
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        // unnormalized input, user was typing this:
        const float old_value = dt_bauhaus_slider_get_val(GTK_WIDGET(darktable.bauhaus->current));
        const float new_value = dt_calculator_solve(old_value, darktable.bauhaus->keys);
        if(isfinite(new_value)) dt_bauhaus_slider_set_val(GTK_WIDGET(darktable.bauhaus->current), new_value);
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Escape)
      {
        // discard input and close popup
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else
        return FALSE;

      return TRUE;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      if(!g_utf8_validate(event->string, -1, NULL)) return FALSE;
      const gunichar c = g_utf8_get_char(event->string);
      const long int char_width = g_utf8_next_char(event->string) - event->string;
      if(darktable.bauhaus->keys_cnt + 1 + char_width < 64 && g_unichar_isprint(c))
      {
        // only accept key input if still valid or editable?
        g_utf8_strncpy(darktable.bauhaus->keys + darktable.bauhaus->keys_cnt, event->string, 1);
        darktable.bauhaus->keys_cnt += char_width;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0
              && (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete))
      {
        darktable.bauhaus->keys_cnt
            -= (darktable.bauhaus->keys + darktable.bauhaus->keys_cnt)
               - g_utf8_prev_char(darktable.bauhaus->keys + darktable.bauhaus->keys_cnt);
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0 && darktable.bauhaus->keys_cnt + 1 < 64
              && (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter))
      {
        // accept unique matches only for editable:
        if(darktable.bauhaus->current->data.combobox.editable)
          darktable.bauhaus->end_mouse_y = FLT_MAX;
        else
          darktable.bauhaus->end_mouse_y = 0;
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        dt_bauhaus_widget_accept(darktable.bauhaus->current, TRUE);
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Escape)
      {
        // discard input and close popup
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Up)
      {
        _combobox_next_sensitive(darktable.bauhaus->current, -1);
      }
      else if(event->keyval == GDK_KEY_Down)
      {
        _combobox_next_sensitive(darktable.bauhaus->current, +1);
      }
      else if(event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
      {
        // return pressed, but didn't type anything
        darktable.bauhaus->end_mouse_y = -1; // negative will use currently highlighted instead.
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_widget_accept(darktable.bauhaus->current, TRUE);
        dt_bauhaus_hide_popup();
      }
      else
        return FALSE;
      return TRUE;
    }
    default:
      return FALSE;
  }
}

static gboolean dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  struct dt_bauhaus_widget_t *w = (struct dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  bauhaus_request_focus(w);

  double event_x = event->x;
  double event_y = event->y;
  double main_width = 0.;
  _bh_active_region_t activated = _bh_get_active_region(widget, &event_x, &event_y, &main_width, NULL);
  darktable.bauhaus->mouse_x = event_x;
  darktable.bauhaus->mouse_y = event_y;

  if(activated == BH_REGION_OUT) return FALSE;

  if(activated == BH_REGION_QUAD)
  {
    dt_bauhaus_widget_press_quad(widget);
    return TRUE;
  }
  else if(activated == BH_REGION_MAIN)
  {
    if(event->button == 1)
    {
      if(event->type == GDK_2BUTTON_PRESS)
      {
        // double left click on the main region : reset value to default
        dt_bauhaus_slider_reset(widget);
        d->is_dragging = 0;
      }
      else
      {
        // single left click on main region : redraw the slider immediately
        // but without committing results to pipeline yet.
        if(event_y < darktable.bauhaus->line_height)
        {
          // single left click on the header name : do nothing (only give focus)
          d->is_dragging = 0;
        }
        else
        {
          // single left click on slider bar : set new value
          d->is_dragging = 1;
          dt_bauhaus_slider_set_normalized(w, event_x / main_width, FALSE, FALSE);
        }
      }
    }
    else if(event->button == 3)
    {
      // right click : show accurate slider setting popup
      d->oldpos = d->pos;
      dt_bauhaus_show_popup(widget);
    }
    else if(event->button == 2)
    {
      // middle click : reset zoom range to soft min/max
      _slider_zoom_range(w, 0);
      _slider_zoom_toast(w);
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_slider_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  struct dt_bauhaus_widget_t *w = (struct dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  dt_bauhaus_widget_release_quad(widget);

  // is_dragging is set TRUE no matter what on button_press, except if it's a double click.
  // double click is the only event handled in button_press, otherwise we assume every event
  // is drag and drop, and handle the final drag coordinate here.
  if(d->is_dragging)
  {
    d->is_dragging = 0;
    if(d->timeout_handle) g_source_remove(d->timeout_handle);
    d->timeout_handle = 0;

    if(event->button == 1)
    {
      dt_bauhaus_slider_set_normalized(w, darktable.bauhaus->mouse_x / _widget_get_main_width(w, NULL, NULL), TRUE, FALSE);
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  struct dt_bauhaus_widget_t *w = (struct dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  _bh_active_region_t activated = BH_REGION_OUT; // = 0

  if(d->is_dragging && event->state & GDK_BUTTON1_MASK)
  {
    double event_x = event->x;
    double event_y = event->y;
    double main_width;
    activated = _bh_get_active_region(widget, &event_x, &event_y, &main_width, NULL);

    darktable.bauhaus->mouse_x = event_x;
    darktable.bauhaus->mouse_y = event_y;
    dt_bauhaus_slider_set_normalized(w, event_x / main_width, FALSE, FALSE);
  }

  return activated;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
