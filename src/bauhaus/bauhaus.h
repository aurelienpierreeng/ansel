/*
    This file is part of darktable,
    Copyright (C) 2012-2021 darktable developers.

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

#include "common/colorlabels.h"
#include "common/gui_module_api.h"
#include "common/introspection.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_BAUHAUS_WIDGET_TYPE dt_bh_get_type()
#define DT_BAUHAUS_WIDGET(obj) G_TYPE_CHECK_INSTANCE_CAST((obj), DT_BAUHAUS_WIDGET_TYPE, DtBauhausWidget)
#define DT_BAUHAUS_WIDGET_CLASS(obj) G_TYPE_CHECK_CLASS_CAST((obj), DT_BAUHAUS_WIDGET, DtBauhausWidgetClass)
#define DT_IS_BAUHAUS_WIDGET(obj) G_TYPE_CHECK_INSTANCE_TYPE((obj), DT_BAUHAUS_WIDGET_TYPE)
#define DT_IS_BAUHAUS_WIDGET_CLASS(obj) G_TYPE_CHECK_CLASS_TYPE((obj), DT_BAUHAUS_WIDGET_TYPE)
#define DT_BAUHAUS_WIDGET_GET_CLASS                                                                          \
  G_TYPE_INSTANCE_GET_CLASS((obj), DT_BAUHAUS_WIDGET_TYPE, DtBauhausWidgetClass)

extern GType DT_BAUHAUS_WIDGET_TYPE;

#define DT_BAUHAUS_SLIDER_VALUE_CHANGED_DELAY_MAX 500
#define DT_BAUHAUS_SLIDER_VALUE_CHANGED_DELAY_MIN 25
#define DT_BAUHAUS_SLIDER_MAX_STOPS 20
#define DT_BAUHAUS_COMBO_MAX_TEXT 180

// INNER_PADDING is the horizontal space between slider and quad
// and vertical space between labels and slider baseline
#define INNER_PADDING DT_PIXEL_APPLY_DPI(4)
#define INTERNAL_PADDING 2. * INNER_PADDING

typedef struct dt_bauhaus_t dt_bauhaus_t;

typedef enum dt_bauhaus_type_t
{
  DT_BAUHAUS_SLIDER = 1,
  DT_BAUHAUS_COMBOBOX = 2,
  // TODO: all the fancy color sliders..
} dt_bauhaus_type_t;

typedef enum dt_bauhaus_curve_t
{
  DT_BAUHAUS_SET = 1,
  DT_BAUHAUS_GET = 2
} dt_bauhaus_curve_t;

// data portion for a slider
typedef struct dt_bauhaus_slider_data_t
{
  float pos;      // normalized slider value
  float oldpos;   // slider value before entering finetune mode (normalized)
  float step;     // step width (not normalized)
  float defpos;   // default value (not normalized)
  float min, max; // min and max range
  float soft_min, soft_max;
  float hard_min, hard_max;
  int digits;  // how many decimals to round to

  float (*grad_col)[3]; // colors for gradient slider
  int grad_cnt;         // how many stops
  float *grad_pos;      // and position of these.

  int fill_feedback : 1; // fill the slider with brighter part up to the handle?

  const char *format;   // numeric value is printed with this format
  float factor;         // multiplication factor before printing
  float offset;         // addition before printing

  gboolean is_dragging;      // indicates is mouse is dragging slider
  guint timeout_handle; // used to store id of timeout routine
} dt_bauhaus_slider_data_t;

typedef enum dt_bauhaus_combobox_alignment_t
{
  DT_BAUHAUS_COMBOBOX_ALIGN_LEFT = 0,
  DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT = 1
} dt_bauhaus_combobox_alignment_t;

// data portion for a combobox
typedef struct dt_bauhaus_combobox_entry_t
{
  char *label;
  dt_bauhaus_combobox_alignment_t alignment;
  gboolean sensitive;
  gpointer data;
  void (*free_func)(gpointer); // callback to free data elements
} dt_bauhaus_combobox_entry_t;

typedef struct dt_bauhaus_combobox_data_t
{
  int active;           // currently active element
  int hovered;          // currently hovered element, to be used by drawings until and if it is set to active
  int defpos;           // default position
  int editable;         // 1 if arbitrary text may be typed
  dt_bauhaus_combobox_alignment_t text_align; // if selected text in combo should be aligned to the left/right
  char *text;           // to hold arbitrary text if editable
  PangoEllipsizeMode entries_ellipsis;
  GPtrArray *entries;
  guint timeout_handle; // used to store id of timeout routine
  void (*populate)(GtkWidget *w, void *module); // function to populate the combo list on the fly
} dt_bauhaus_combobox_data_t;

typedef union dt_bauhaus_data_t
{
  // this is the placeholder for the data portions
  // associated with the implementations such as
  // sliders, combo boxes, ..
  dt_bauhaus_slider_data_t slider;
  dt_bauhaus_combobox_data_t combobox;
} dt_bauhaus_data_t;

// gah, caps.
typedef struct dt_bauhaus_widget_t DtBauhausWidget;
typedef struct dt_bauhaus_widget_class_t DtBauhausWidgetClass;

typedef void (*dt_bauhaus_quad_paint_f)(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

// our new widget and its private members, inheriting from drawing area:
typedef struct dt_bauhaus_widget_t
{
  // gtk base widget
  GtkDrawingArea parent;
  // which type of control
  dt_bauhaus_type_t type;
  // associated image operation module (to handle focus and such)
  dt_gui_module_t *module;
  // pointer to iop field linked to widget
  gpointer field;
  // type of field
  dt_introspection_type_t field_type;

  // label text, short
  char label[256];
  // callback function to draw the quad icon
  dt_bauhaus_quad_paint_f quad_paint;
  // minimal modifiers for paint function.
  int quad_paint_flags;
  // data for the paint callback
  void *quad_paint_data;
  // quad is a toggle button?
  int quad_toggle;
  // show quad icon or space
  gboolean show_quad;

  // horizontally expand to fit container's width. Set to TRUE by default,
  // assuming vertical columns. Manually set to FALSE if using in toolbar menu.
  gboolean expand;

  // margin and padding structure, defined in css, retrieve on each draw
  GtkBorder *margin, *padding;

  int timeout;

  // TRUE if accels should not be enabled here.
  // Use that for blending
  gboolean no_accels;

  // Reference to the global bauhaus structure holding common styles and such.
  dt_bauhaus_t *bauhaus;

  // Use the app-wise default value-changed callback from *bauhaus
  // or use custom implementation
  gboolean use_default_callback;

  // goes last, might extend past the end:
  dt_bauhaus_data_t data;

} dt_bauhaus_widget_t;


// global static data:
enum
{
  DT_BAUHAUS_VALUE_CHANGED_SIGNAL,
  DT_BAUHAUS_QUAD_PRESSED_SIGNAL,
  DT_BAUHAUS_LAST_SIGNAL
};


// class of our new widget, inheriting from drawing area
typedef struct dt_bauhaus_widget_class_t
{
  GtkDrawingAreaClass parent_class;

  // our custom signals
  guint signals[DT_BAUHAUS_LAST_SIGNAL];
} dt_bauhaus_widget_class_t;

struct dt_bauhaus_t
{
  // The bauhaus widget popup is shared across widgets,
  // so we need to track which one is currently capturing it
  struct dt_bauhaus_widget_t *current;
  GtkWidget *popup_window;
  GtkWidget *popup_area;

  // are set by the motion notification, to be used during drawing.
  float mouse_x, mouse_y;

  // time when the popup window was opened. this is sortof a hack to
  // detect `double clicks between windows' to reset the combobox.
  guint32 opentime;
  // pointer position when popup window is closed
  float end_mouse_x, end_mouse_y;
  // used to determine whether the user crossed the line already.
  int change_active;
  float mouse_line_distance;
  // key input buffer
  char keys[64];
  int keys_cnt;

  // flag set on button press indicating that popup should be hidden in button release handler
  gboolean hiding;

  // appearance relevant stuff:
  // sizes and fonts:
  float line_height;                     // height of a line of text
  float marker_size;                     // height of the slider indicator
  float baseline_size;                   // height of the slider bar
  float border_width;                    // width of the border of the slider marker
  float quad_width;                      // width of the quad area to paint icons
  PangoFontDescription *pango_font_desc; // no need to recreate this for every string we want to print

  // colors for sliders and comboboxes
  GdkRGBA color_fg, color_fg_insensitive, color_bg, color_border, indicator_border, color_fill;

  // colors for graphs
  GdkRGBA graph_bg, graph_exterior, graph_border, graph_fg, graph_grid, graph_fg_active, graph_overlay, inset_histogram;
  GdkRGBA graph_colors[3];               // primaries
  GdkRGBA colorlabels[DT_COLORLABELS_LAST];

  // View-wise default callback to wire to the value-changed signal
  // if the widget declaration sets the boolean
  void (*default_value_changed_callback)(GtkWidget *widget);
};

#define DT_BAUHAUS_SPACE 0


dt_bauhaus_t * dt_bauhaus_init();
void dt_bauhaus_cleanup(dt_bauhaus_t *bauhaus);

// load theme colors, fonts, etc
void dt_bauhaus_load_theme(dt_bauhaus_t *bauhaus);

// common functions:
// set the label text:
void dt_bauhaus_widget_set_label(GtkWidget *w, const char *label);
const char* dt_bauhaus_widget_get_label(GtkWidget *w);
// attach a custom painted quad to the space at the right side (overwriting the default icon if any):
void dt_bauhaus_widget_set_quad_paint(GtkWidget *w, dt_bauhaus_quad_paint_f f, int paint_flags, void *paint_data);
// make this quad a toggle button:
void dt_bauhaus_widget_set_quad_toggle(GtkWidget *w, int toggle);
// set active status for the quad toggle button:
void dt_bauhaus_widget_set_quad_active(GtkWidget *w, int active);
// get active status for the quad toggle button:
int dt_bauhaus_widget_get_quad_active(GtkWidget *w);
// set quad visibility:
void dt_bauhaus_widget_set_quad_visibility(GtkWidget *w, const gboolean visible);
// set pointer to iop params field:
void dt_bauhaus_widget_set_field(GtkWidget *w, gpointer field, dt_introspection_type_t field_type);

void dt_bauhaus_hide_popup(dt_bauhaus_t *bh);
void dt_bauhaus_show_popup(GtkWidget *w);

// slider:
GtkWidget *dt_bauhaus_slider_new(dt_bauhaus_t *bh, dt_gui_module_t *self);
GtkWidget *dt_bauhaus_slider_new_with_range(dt_bauhaus_t *bh, dt_gui_module_t *self, float min, float max, float step,
                                            float defval, int digits);
GtkWidget *dt_bauhaus_slider_new_with_range_and_feedback(dt_bauhaus_t *bh, dt_gui_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback);

GtkWidget *dt_bauhaus_slider_from_widget(dt_bauhaus_t *bh, dt_bauhaus_widget_t *widget, dt_gui_module_t *self, float min, float max,
                                         float step, float defval, int digits, int feedback);

// outside doesn't see the real type, we cast it internally.
void dt_bauhaus_slider_set(GtkWidget *w, float pos);
void dt_bauhaus_slider_set_val(GtkWidget *w, float val);
float dt_bauhaus_slider_get(GtkWidget *w);
float dt_bauhaus_slider_get_val(GtkWidget *w);
char *dt_bauhaus_slider_get_text(GtkWidget *w, float val);

void dt_bauhaus_slider_set_soft_min(GtkWidget* w, float val);
float dt_bauhaus_slider_get_soft_min(GtkWidget* w);
void dt_bauhaus_slider_set_soft_max(GtkWidget* w, float val);
float dt_bauhaus_slider_get_soft_max(GtkWidget* w);
void dt_bauhaus_slider_set_soft_range(GtkWidget *widget, float soft_min, float soft_max);

void dt_bauhaus_slider_set_hard_min(GtkWidget* w, float val);
float dt_bauhaus_slider_get_hard_min(GtkWidget* w);
void dt_bauhaus_slider_set_hard_max(GtkWidget* w, float val);
float dt_bauhaus_slider_get_hard_max(GtkWidget* w);

void dt_bauhaus_slider_set_digits(GtkWidget *w, int val);
int dt_bauhaus_slider_get_digits(GtkWidget *w);
void dt_bauhaus_slider_set_step(GtkWidget *w, float val);
float dt_bauhaus_slider_get_step(GtkWidget *w);

void dt_bauhaus_slider_set_feedback(GtkWidget *w, int feedback);

void dt_bauhaus_slider_reset(GtkWidget *widget);
void dt_bauhaus_slider_set_format(GtkWidget *w, const char *format);
void dt_bauhaus_slider_set_factor(GtkWidget *w, float factor);
void dt_bauhaus_slider_set_offset(GtkWidget *w, float offset);
void dt_bauhaus_slider_set_stop(GtkWidget *widget, float stop, float r, float g, float b);
void dt_bauhaus_slider_clear_stops(GtkWidget *widget);
void dt_bauhaus_slider_set_default(GtkWidget *widget, float def);

// combobox:
void dt_bauhaus_combobox_from_widget(dt_bauhaus_t *bh, dt_bauhaus_widget_t* widget,dt_gui_module_t *self);
GtkWidget *dt_bauhaus_combobox_new(dt_bauhaus_t *bh, dt_gui_module_t *self);
GtkWidget *dt_bauhaus_combobox_new_full(dt_bauhaus_t *bh, dt_gui_module_t *self,
                                        const char *label, const char *tip, int pos, GtkCallback callback,
                                        gpointer data, const char **texts);

#define DT_BAUHAUS_COMBOBOX_NEW_FULL(bauhaus, widget, action,  label, tip, pos, callback, data, ...)         \
{                                                                                                            \
  static const gchar *texts[] = { __VA_ARGS__, NULL };                                                       \
  widget = dt_bauhaus_combobox_new_full(bauhaus, action, label, tip, pos, callback, data, texts);            \
}

void dt_bauhaus_combobox_add(GtkWidget *widget, const char *text);
void dt_bauhaus_combobox_add_aligned(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align);
void dt_bauhaus_combobox_add_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                  gpointer data, void (*free_func)(void *data), gboolean sensitive);
void dt_bauhaus_combobox_set(GtkWidget *w, int pos);
gboolean dt_bauhaus_combobox_set_from_text(GtkWidget *w, const char *text);
gboolean dt_bauhaus_combobox_set_from_value(GtkWidget *w, int value);
void dt_bauhaus_combobox_remove_at(GtkWidget *widget, int pos);
void dt_bauhaus_combobox_insert(GtkWidget *widget, const char *text,int pos);
void dt_bauhaus_combobox_insert_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                     gpointer data, void (*free_func)(void *data), int pos);
int dt_bauhaus_combobox_length(GtkWidget *widget);
void dt_bauhaus_combobox_set_editable(GtkWidget *w, int editable);
void dt_bauhaus_combobox_set_selected_text_align(GtkWidget *widget, const dt_bauhaus_combobox_alignment_t text_align);
int dt_bauhaus_combobox_get_editable(GtkWidget *w);
const char *dt_bauhaus_combobox_get_text(GtkWidget *w);
void dt_bauhaus_combobox_set_text(GtkWidget *w, const char *text);
int dt_bauhaus_combobox_get(GtkWidget *w);
const char *dt_bauhaus_combobox_get_entry(GtkWidget *w, int pos);
gpointer dt_bauhaus_combobox_get_data(GtkWidget *widget);
void dt_bauhaus_combobox_clear(GtkWidget *w);
void dt_bauhaus_combobox_set_default(GtkWidget *widget, int def);
void dt_bauhaus_combobox_add_populate_fct(GtkWidget *widget, void (*fct)(GtkWidget *w, void *module));
void dt_bauhaus_combobox_add_list(GtkWidget *widget, const char **texts);
void dt_bauhaus_combobox_entry_set_sensitive(GtkWidget *widget, int pos, gboolean sensitive);
void dt_bauhaus_combobox_set_entries_ellipsis(GtkWidget *widget, PangoEllipsizeMode ellipis);

/* Disable accels for this widget.
* WARNING: accels are inited when setting the widget label. This function should be called before.
* It will be useless for the "one-line" init & setter functions.
*/
void dt_bauhaus_disable_accels(GtkWidget *widget);

static inline void set_color(cairo_t *cr, GdkRGBA color)
{
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
}

/**
 * @brief Tell the widget to use the globally-defined default callback in the bauhaus structure
 * This callback needs to be defined first, of course.
 *
 * @param widget
 */
void dt_bauhaus_set_use_default_callback(GtkWidget *widget);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
