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

/**
 * @file libs/tools/canvas_toolbar.c
 * @brief The Canvas atelier's toolbar: file and object menus, creation buttons, the guides popover.
 *
 * @details The toolbar holds no document state. Every button asks the view for an action
 * through `proxy.canvas` (see canvas/canvas_actions.h) and the controls that mirror
 * document settings -- the guides popover, the default border, the background -- are
 * refilled from the document on DT_SIGNAL_CANVAS_CHANGED, with their handlers blocked so
 * a refill never writes back.
 */

#include "canvas/canvas.h"
#include "canvas/canvas_actions.h"
#include "common/conf.h"
#include "common/module_versioning.h"
#include "control/signal.h"
#include "gui/window_manager.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "views/view.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

DT_MODULE(1)

typedef struct dt_lib_canvas_toolbar_t
{
  GtkWidget *connect_toggle;
  // the guides popover
  GtkWidget *grid_show;
  GtkWidget *grid_snap;
  GtkWidget *grid_size;
  GtkWidget *grid_color;
  GtkWidget *page_show;
  GtkWidget *page_over;
  GtkWidget *page_snap;
  GtkWidget *page_size;
  GtkWidget *page_orientation;
  GtkWidget *page_color;
  GtkWidget *gutter_snap;
  GtkWidget *gutter_size;
  GtkWidget *margin_show;
  GtkWidget *margin_snap;
  GtkWidget *margin_size;
  GtkWidget *margin_color;
  GtkWidget *bleed_show;
  GtkWidget *bleed_snap;
  GtkWidget *bleed_size;
  GtkWidget *bleed_color;
  GtkWidget *gutter_show;
  GtkWidget *gutter_color;
  GtkWidget *size_snap;
  // the shadow popover
  GtkWidget *texture_contrast;
  GtkWidget *texture_detail;
  GtkWidget *texture_scale;
  GtkWidget *texture_grain;
  GtkWidget *shadow_offset_x;
  GtkWidget *shadow_offset_y;
  GtkWidget *shadow_blur;
  GtkWidget *shadow_color;
  // the rest of the row
  GtkWidget *background_color;
  GtkWidget *background_style;
  GtkWidget *border_width;
  GtkWidget *border_color;
  GtkWidget *corner_radius;
  GtkWidget *layout;
  GtkWidget *sort;
  gboolean refilling; ///< handlers ignore changes while the controls are refilled from the document
} dt_lib_canvas_toolbar_t;

const char *name(dt_lib_module_t *self)
{
  return _("Canvas");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "canvas", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

/* --- talking to the view -------------------------------------------------------------- */

static dt_view_t *_canvas_view(void)
{
  return dt_view_manager_get_global()->proxy.canvas.view;
}

static const dt_canvas_t *_document(void)
{
  dt_view_t *view = _canvas_view();
  if(IS_NULL_PTR(view) || IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.document)) return NULL;
  return dt_view_manager_get_global()->proxy.canvas.document(view);
}

static void _ask(const dt_canvas_action_t action)
{
  dt_view_t *view = _canvas_view();
  if(IS_NULL_PTR(view) || IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.action)) return;
  dt_view_manager_get_global()->proxy.canvas.action(view, action);
}

static void _action_clicked(GtkWidget *widget, gpointer user_data)
{
  _ask((dt_canvas_action_t)GPOINTER_TO_INT(user_data));
}

static gboolean _live(dt_lib_module_t *self, dt_view_t **view)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  if(toolbar->refilling) return FALSE;
  *view = _canvas_view();
  return !IS_NULL_PTR(*view);
}

static void _rgba_of(GtkWidget *button, float rgba[4])
{
  GdkRGBA color;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
  rgba[0] = (float)color.red;
  rgba[1] = (float)color.green;
  rgba[2] = (float)color.blue;
  rgba[3] = (float)color.alpha;
}

static void _rgba_to(GtkWidget *button, const dt_canvas_color_t *color, const gboolean with_alpha)
{
  GdkRGBA rgba = { color->red, color->green, color->blue, with_alpha ? color->alpha : 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &rgba);
}

/* --- handlers ------------------------------------------------------------------------- */

static void _connect_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live((dt_lib_module_t *)user_data, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.is_connecting)) return;
  if(gtk_toggle_button_get_active(button) != dt_view_manager_get_global()->proxy.canvas.is_connecting(view))
    _ask(DT_CANVAS_ACTION_CONNECT_MODE);
}

/** A guides checkbox: its flag bit is in "guide-flag". */
static void _guide_flag_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live((dt_lib_module_t *)user_data, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_guides)) return;
  const int flag = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "guide-flag"));
  dt_view_manager_get_global()->proxy.canvas.set_guides(view, flag, gtk_toggle_button_get_active(button) ? flag : 0);
}

static void _grid_size_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live((dt_lib_module_t *)user_data, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_grid_size)) return;
  dt_view_manager_get_global()->proxy.canvas.set_grid_size(view, (float)gtk_spin_button_get_value(spin));
}

static void _gutter_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live((dt_lib_module_t *)user_data, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_gutter)) return;
  dt_view_manager_get_global()->proxy.canvas.set_gutter(view, (float)gtk_spin_button_get_value(spin));
}

static void _grid_color_set(GtkWidget *button, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live((dt_lib_module_t *)user_data, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_grid_color)) return;
  float rgba[4];
  _rgba_of(button, rgba);
  dt_view_manager_get_global()->proxy.canvas.set_grid_color(view, rgba);
}

static void _page_color_set(GtkWidget *button, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live((dt_lib_module_t *)user_data, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_page_color)) return;
  float rgba[4];
  _rgba_of(button, rgba);
  dt_view_manager_get_global()->proxy.canvas.set_page_color(view, rgba);
}

static void _gutter_color_set(GtkWidget *button, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live((dt_lib_module_t *)user_data, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_gutter_color)) return;
  float rgba[4];
  _rgba_of(button, rgba);
  dt_view_manager_get_global()->proxy.canvas.set_gutter_color(view, rgba);
}

/** Any shadow control: the whole shadow is read back and applied as the canvas default. */
static void _shadow_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_shadow)) return;
  float rgba[4];
  _rgba_of(toolbar->shadow_color, rgba);
  dt_view_manager_get_global()->proxy.canvas.set_shadow(
      view, rgba, (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->shadow_offset_x)),
      (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->shadow_offset_y)),
      (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->shadow_blur)));
}

static void _page_guides_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_page_guides)) return;
  dt_view_manager_get_global()->proxy.canvas.set_page_guides(
      view, (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->margin_size)),
      (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->bleed_size)));
}

static void _margin_color_set(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_margin_color)) return;
  float rgba[4];
  _rgba_of(widget, rgba);
  dt_view_manager_get_global()->proxy.canvas.set_margin_color(view, rgba);
}

static void _bleed_color_set(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_bleed_color)) return;
  float rgba[4];
  _rgba_of(widget, rgba);
  dt_view_manager_get_global()->proxy.canvas.set_bleed_color(view, rgba);
}

static void _page_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_paper)) return;
  // The list's order is not the stored value: a size appended to the enum shows where it belongs.
  const int position = gtk_combo_box_get_active(GTK_COMBO_BOX(toolbar->page_size));
  dt_view_manager_get_global()->proxy.canvas.set_paper(view, (int)dt_canvas_paper_code(position),
                                                     gtk_combo_box_get_active(GTK_COMBO_BOX(toolbar->page_orientation)));
}

/** The background's colour, or its style: each sends only what it owns, so a paper can bring its own colour. */
static void _background_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_background)) return;
  if(widget == toolbar->background_style)
  {
    // The list's order is not the stored value: a background appended to the enum shows where
    // it belongs, which is how Transparent came to head a list it joined last.
    const int position = gtk_combo_box_get_active(GTK_COMBO_BOX(toolbar->background_style));
    dt_view_manager_get_global()->proxy.canvas.set_background(view, NULL, (int)dt_canvas_background_code(position));
    return;
  }
  float rgba[4];
  _rgba_of(toolbar->background_color, rgba);
  dt_view_manager_get_global()->proxy.canvas.set_background(view, rgba, -1);
}

static void _refill(dt_lib_module_t *self);

static void _texture_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_texture)) return;
  dt_view_manager_get_global()->proxy.canvas.set_texture(
      view, (float)gtk_range_get_value(GTK_RANGE(toolbar->texture_contrast)),
      (float)gtk_range_get_value(GTK_RANGE(toolbar->texture_detail)),
      (float)gtk_range_get_value(GTK_RANGE(toolbar->texture_scale)),
      (float)gtk_range_get_value(GTK_RANGE(toolbar->texture_grain)));
}

static void _texture_reset(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_texture)) return;
  dt_view_manager_get_global()->proxy.canvas.set_texture(view, 1.0f, 1.0f, 1.0f, 1.0f);
  // Every other caller of set_texture is one of the four sliders sending its own value, and
  // refilling under a slider the user is still holding would fight the pointer -- so the
  // setter stays quiet and the ONE caller that writes all four behind their backs refreshes
  // them itself. Without it the reset reached the document and nothing else: the sliders
  // kept the old positions, so it read as doing nothing at all, and the next touch of any
  // slider sent all four stale values back and undid it.
  _refill(self);
}

static void _border_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(self, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_border)) return;
  float rgba[4];
  _rgba_of(toolbar->border_color, rgba);
  dt_view_manager_get_global()->proxy.canvas.set_border(view, rgba, (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->border_width)));
}

static void _corner_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live((dt_lib_module_t *)user_data, &view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_corner_radius)) return;
  dt_view_manager_get_global()->proxy.canvas.set_corner_radius(view, (float)gtk_spin_button_get_value(spin));
}

static void _sort_changed(GtkComboBox *combo, gpointer user_data)
{
  dt_conf_set_int("canvas/layout_sort", CLAMP(gtk_combo_box_get_active(combo), 0, DT_CANVAS_SORT_LAST - 1));
}

static void _layout_apply(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  static const dt_canvas_action_t layouts[] = { DT_CANVAS_ACTION_LAYOUT_GRID, DT_CANVAS_ACTION_LAYOUT_MASONRY,
                                                DT_CANVAS_ACTION_LAYOUT_ROW, DT_CANVAS_ACTION_LAYOUT_COLUMN };
  const int choice = gtk_combo_box_get_active(GTK_COMBO_BOX(toolbar->layout));
  if(choice < 0 || choice >= (int)G_N_ELEMENTS(layouts)) return;
  _ask(layouts[choice]);
}

/* --- refilling from the document --------------------------------------------------------- */

static void _refill(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  const dt_canvas_t *canvas = _document();
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(toolbar)) return;
  toolbar->refilling = TRUE;
  dt_view_t *view = _canvas_view();
  const gboolean connecting = !IS_NULL_PTR(view) && !IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.is_connecting)
                              && dt_view_manager_get_global()->proxy.canvas.is_connecting(view);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->connect_toggle), connecting);

  const uint32_t flags = canvas->grid_flags;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->grid_show), (flags & DT_CANVAS_GRID_VISIBLE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->grid_snap), (flags & DT_CANVAS_GRID_SNAP) != 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->grid_size), canvas->grid_size);
  _rgba_to(toolbar->grid_color, &canvas->grid_color, TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->page_show), (flags & DT_CANVAS_PAGE_VISIBLE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->page_snap), (flags & DT_CANVAS_SNAP_PAGE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->page_over), (flags & DT_CANVAS_GUIDES_OVER) != 0);
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->page_size), dt_canvas_paper_position(canvas->paper_size));
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->page_orientation), canvas->paper_landscape ? 1 : 0);
  _rgba_to(toolbar->page_color, &canvas->page_color, TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->gutter_snap), (flags & DT_CANVAS_SNAP_GUTTER) != 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->gutter_size), canvas->gutter);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->size_snap), (flags & DT_CANVAS_SNAP_SIZE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->gutter_show), (flags & DT_CANVAS_GUTTER_VISIBLE) != 0);
  _rgba_to(toolbar->gutter_color, &canvas->gutter_color, TRUE);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->shadow_offset_x), canvas->shadow.offset_x);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->shadow_offset_y), canvas->shadow.offset_y);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->shadow_blur), canvas->shadow.blur);
  _rgba_to(toolbar->shadow_color, &canvas->shadow.color, TRUE);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->border_width), canvas->border_width);
  _rgba_to(toolbar->border_color, &canvas->border_color, TRUE);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->corner_radius), canvas->corner_radius);
  _rgba_to(toolbar->background_color, &canvas->background, FALSE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->background_style),
                           dt_canvas_background_position(canvas->background_style));
  float contrast = 1.0f;
  float detail = 1.0f;
  float scale = 1.0f;
  float grain = 1.0f;
  dt_canvas_texture_get(canvas, &contrast, &detail, &scale, &grain);
  gtk_range_set_value(GTK_RANGE(toolbar->texture_contrast), contrast);
  gtk_range_set_value(GTK_RANGE(toolbar->texture_detail), detail);
  gtk_range_set_value(GTK_RANGE(toolbar->texture_scale), scale);
  gtk_range_set_value(GTK_RANGE(toolbar->texture_grain), grain);
  toolbar->refilling = FALSE;
}

static void _canvas_changed(gpointer instance, gpointer user_data)
{
  _refill((dt_lib_module_t *)user_data);
}

/* --- building ------------------------------------------------------------------------------ */

static GtkWidget *_button(GtkWidget *box, const char *label, const char *tooltip, const dt_canvas_action_t action)
{
  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect(button, "clicked", G_CALLBACK(_action_clicked), GINT_TO_POINTER(action));
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
  return button;
}

static void _separator(GtkWidget *box)
{
  GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_margin_start(separator, DT_PIXEL_APPLY_DPI(6));
  gtk_widget_set_margin_end(separator, DT_PIXEL_APPLY_DPI(6));
  gtk_box_pack_start(GTK_BOX(box), separator, FALSE, FALSE, 0);
}

static GtkWidget *_menu_entry(GtkWidget *menu, const char *label, GCallback callback, gpointer data)
{
  GtkWidget *item = gtk_menu_item_new_with_label(label);
  g_signal_connect(item, "activate", callback, data);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  return item;
}

static void _action_item(GtkWidget *menu, const char *label, const dt_canvas_action_t action)
{
  _menu_entry(menu, label, G_CALLBACK(_action_clicked), GINT_TO_POINTER(action));
}

/** A toolbar button that drops a menu. */
/** A menu button styled as a text label with an ellipsis: what opens something else, not an action. */
static GtkWidget *_flat_menu_button(GtkWidget *box, const char *label, const char *tooltip)
{
  GtkWidget *button = gtk_menu_button_new();
  gchar *text = g_strdup_printf("%s\xe2\x80\xa6", label);
  gtk_button_set_label(GTK_BUTTON(button), text);
  dt_free(text);
  gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
  return button;
}

static GtkWidget *_menu_button(GtkWidget *box, const char *label, const char *tooltip, GtkWidget *menu)
{
  GtkWidget *button = _flat_menu_button(box, label, tooltip);
  gtk_widget_show_all(menu);
  gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
  return button;
}

static GtkWidget *_popover_button(GtkWidget *box, const char *label, const char *tooltip, GtkWidget *popover)
{
  GtkWidget *button = _flat_menu_button(box, label, tooltip);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
  return button;
}

/** A slider row of a popover: a label, a scale, its value. */
static GtkWidget *_popover_slider(GtkWidget *grid, const int row, const char *label, const double low,
                                  const double high, const double step, const char *tooltip, GCallback callback,
                                  gpointer data)
{
  GtkWidget *name = gtk_label_new(label);
  gtk_widget_set_halign(name, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), name, 0, row, 1, 1);
  GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, low, high, step);
  gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
  gtk_scale_set_digits(GTK_SCALE(scale), 2);
  gtk_widget_set_size_request(scale, DT_PIXEL_APPLY_DPI(220), -1);
  gtk_widget_set_hexpand(scale, TRUE);
  gtk_widget_set_tooltip_text(scale, tooltip);
  g_signal_connect(scale, "value-changed", callback, data);
  gtk_grid_attach(GTK_GRID(grid), scale, 1, row, 3, 1);
  return scale;
}

/** A guides checkbox bound to one flag bit. */
static GtkWidget *_guide_check(dt_lib_module_t *self, GtkWidget *grid, const int row, const int col,
                               const char *label, const int flag)
{
  GtkWidget *check = gtk_check_button_new_with_label(label);
  g_object_set_data(G_OBJECT(check), "guide-flag", GINT_TO_POINTER(flag));
  g_signal_connect(check, "toggled", G_CALLBACK(_guide_flag_toggled), self);
  gtk_grid_attach(GTK_GRID(grid), check, col, row, 1, 1);
  return check;
}

static GtkWidget *_section_label(GtkWidget *grid, const int row, const char *text)
{
  GtkWidget *label = gtk_label_new(NULL);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  dt_free(markup);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_widget_set_margin_top(label, DT_PIXEL_APPLY_DPI(row == 0 ? 0 : 8));
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 4, 1);
  return label;
}

static GtkWidget *_labelled(GtkWidget *grid, const int row, const int col, const char *label, GtkWidget *widget)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), box, col, row, 1, 1);
  return widget;
}

/** The guides popover: the grid, the page borders and the gutters, each with its show, snap, size and colour. */
static GtkWidget *_guides_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(4));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_container_set_border_width(GTK_CONTAINER(grid), DT_PIXEL_APPLY_DPI(10));

  _section_label(grid, 0, _("Grid"));
  toolbar->grid_show = _guide_check(self, grid, 1, 0, _("Show"), DT_CANVAS_GRID_VISIBLE);
  toolbar->grid_snap = _guide_check(self, grid, 1, 1, _("Snap"), DT_CANVAS_GRID_SNAP);
  toolbar->grid_size = gtk_spin_button_new_with_range(5.0, 1000.0, 5.0);
  gtk_widget_set_tooltip_text(toolbar->grid_size, _("Grid spacing, in canvas units"));
  g_signal_connect(toolbar->grid_size, "value-changed", G_CALLBACK(_grid_size_changed), self);
  _labelled(grid, 1, 2, _("Size"), toolbar->grid_size);
  toolbar->grid_color = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(toolbar->grid_color), TRUE);
  gtk_widget_set_tooltip_text(toolbar->grid_color, _("Colour of the grid dots"));
  g_signal_connect(toolbar->grid_color, "color-set", G_CALLBACK(_grid_color_set), self);
  _labelled(grid, 1, 3, _("Colour"), toolbar->grid_color);

  _section_label(grid, 2, _("Page borders"));
  toolbar->page_show = _guide_check(self, grid, 3, 0, _("Show"), DT_CANVAS_PAGE_VISIBLE);
  toolbar->page_snap = _guide_check(self, grid, 3, 1, _("Snap"), DT_CANVAS_SNAP_PAGE);
  toolbar->page_over = _guide_check(self, grid, 4, 0, _("Over"), DT_CANVAS_GUIDES_OVER);
  gtk_widget_set_tooltip_text(toolbar->page_over,
                              _("Draw the page borders, margins and bleed over the content rather than under it, so a "
                                "frame that crosses a page break can still be placed against them"));
  toolbar->page_size = gtk_combo_box_text_new();
  for(int position = 0; position < dt_canvas_paper_count(); position++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->page_size), dt_canvas_paper_name(position));
  gtk_widget_set_tooltip_text(toolbar->page_size,
                              _("Divide the canvas into pages of this size, one exported page each. One canvas unit is one "
                                "point, so a print size is its size in points and a screen size is its size in pixels at 72 dpi."));
  g_signal_connect(toolbar->page_size, "changed", G_CALLBACK(_page_changed), self);
  _labelled(grid, 3, 2, _("Size"), toolbar->page_size);
  toolbar->page_color = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(toolbar->page_color), TRUE);
  gtk_widget_set_tooltip_text(toolbar->page_color, _("Colour of the page borders"));
  g_signal_connect(toolbar->page_color, "color-set", G_CALLBACK(_page_color_set), self);
  _labelled(grid, 3, 3, _("Colour"), toolbar->page_color);
  toolbar->page_orientation = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->page_orientation), _("Portrait"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->page_orientation), _("Landscape"));
  g_signal_connect(toolbar->page_orientation, "changed", G_CALLBACK(_page_changed), self);
  _labelled(grid, 4, 2, _("Orientation"), toolbar->page_orientation);

  _section_label(grid, 5, _("Page margins"));
  toolbar->margin_show = _guide_check(self, grid, 6, 0, _("Show"), DT_CANVAS_MARGIN_VISIBLE);
  toolbar->margin_snap = _guide_check(self, grid, 6, 1, _("Snap"), DT_CANVAS_SNAP_MARGIN);
  toolbar->margin_size = gtk_spin_button_new_with_range(0.0, 2000.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->margin_size,
                              _("Kept clear inside every page edge, in canvas units. A guide and a snapping rule only: "
                                "nothing is moved and the page is unchanged."));
  g_signal_connect(toolbar->margin_size, "value-changed", G_CALLBACK(_page_guides_changed), self);
  _labelled(grid, 6, 2, _("Size"), toolbar->margin_size);
  toolbar->margin_color = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(toolbar->margin_color), TRUE);
  gtk_widget_set_tooltip_text(toolbar->margin_color, _("Colour of the margin lines"));
  g_signal_connect(toolbar->margin_color, "color-set", G_CALLBACK(_margin_color_set), self);
  _labelled(grid, 6, 3, _("Colour"), toolbar->margin_color);

  _section_label(grid, 7, _("Bleed"));
  toolbar->bleed_show = _guide_check(self, grid, 8, 0, _("Show"), DT_CANVAS_BLEED_VISIBLE);
  toolbar->bleed_snap = _guide_check(self, grid, 8, 1, _("Snap"), DT_CANVAS_SNAP_BLEED);
  toolbar->bleed_size = gtk_spin_button_new_with_range(0.0, 2000.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->bleed_size,
                              _("How far past every page edge the sheet keeps going, in canvas units. A frame a page break "
                                "cuts in two carries on into the bleed on both sheets, which is what a binding folds around "
                                "and a trim cuts into. The export writes it."));
  g_signal_connect(toolbar->bleed_size, "value-changed", G_CALLBACK(_page_guides_changed), self);
  _labelled(grid, 8, 2, _("Size"), toolbar->bleed_size);
  toolbar->bleed_color = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(toolbar->bleed_color), TRUE);
  gtk_widget_set_tooltip_text(toolbar->bleed_color, _("Colour of the bleed lines"));
  g_signal_connect(toolbar->bleed_color, "color-set", G_CALLBACK(_bleed_color_set), self);
  _labelled(grid, 8, 3, _("Colour"), toolbar->bleed_color);

  _section_label(grid, 9, _("Gutters"));
  toolbar->gutter_show = _guide_check(self, grid, 10, 0, _("Show"), DT_CANVAS_GUTTER_VISIBLE);
  gtk_widget_set_tooltip_text(toolbar->gutter_show,
                              _("Draw each frame's clear margin around it. Two frames snapped side by side meet on one shared line, two gutters apart."));
  toolbar->gutter_snap = _guide_check(self, grid, 10, 1, _("Snap"), DT_CANVAS_SNAP_GUTTER);
  toolbar->gutter_size = gtk_spin_button_new_with_range(0.0, 500.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->gutter_size,
                              _("The clear margin every frame keeps around itself, in canvas units. Side by side, two frames are two of these apart."));
  g_signal_connect(toolbar->gutter_size, "value-changed", G_CALLBACK(_gutter_changed), self);
  _labelled(grid, 10, 2, _("Size"), toolbar->gutter_size);
  toolbar->gutter_color = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(toolbar->gutter_color), TRUE);
  gtk_widget_set_tooltip_text(toolbar->gutter_color, _("Colour of the gutter frames"));
  g_signal_connect(toolbar->gutter_color, "color-set", G_CALLBACK(_gutter_color_set), self);
  _labelled(grid, 10, 3, _("Colour"), toolbar->gutter_color);
  toolbar->size_snap = _guide_check(self, grid, 11, 0, _("Snap sizes to neighbours"), DT_CANVAS_SNAP_SIZE);
  gtk_widget_set_hexpand(toolbar->size_snap, TRUE);

  GtkWidget *popover = gtk_popover_new(NULL);
  gtk_container_add(GTK_CONTAINER(popover), grid);
  gtk_widget_show_all(grid);
  return popover;
}

/** The shadow popover: the default drop shadow of every object that has no shadow of its own. */
static GtkWidget *_shadow_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(4));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_container_set_border_width(GTK_CONTAINER(grid), DT_PIXEL_APPLY_DPI(10));
  _section_label(grid, 0, _("Shadow"));
  toolbar->shadow_offset_x = gtk_spin_button_new_with_range(-500.0, 500.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->shadow_offset_x, _("Offset to the right, in canvas units"));
  g_signal_connect(toolbar->shadow_offset_x, "value-changed", G_CALLBACK(_shadow_changed), self);
  _labelled(grid, 1, 1, _("Right"), toolbar->shadow_offset_x);
  toolbar->shadow_offset_y = gtk_spin_button_new_with_range(-500.0, 500.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->shadow_offset_y, _("Offset downwards, in canvas units"));
  g_signal_connect(toolbar->shadow_offset_y, "value-changed", G_CALLBACK(_shadow_changed), self);
  _labelled(grid, 1, 2, _("Down"), toolbar->shadow_offset_y);
  toolbar->shadow_blur = gtk_spin_button_new_with_range(-500.0, 500.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->shadow_blur,
                              _("Radius, in canvas units: 0 is no shadow, positive drops it outside every object, negative casts it inside along their edges. An object's own bar can override it."));
  g_signal_connect(toolbar->shadow_blur, "value-changed", G_CALLBACK(_shadow_changed), self);
  _labelled(grid, 2, 1, _("Radius"), toolbar->shadow_blur);
  toolbar->shadow_color = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(toolbar->shadow_color), TRUE);
  gtk_widget_set_tooltip_text(toolbar->shadow_color, _("Colour and strength of the shadow"));
  g_signal_connect(toolbar->shadow_color, "color-set", G_CALLBACK(_shadow_changed), self);
  _labelled(grid, 2, 2, _("Colour"), toolbar->shadow_color);
  GtkWidget *popover = gtk_popover_new(NULL);
  gtk_container_add(GTK_CONTAINER(popover), grid);
  gtk_widget_show_all(grid);
  return popover;
}

/** The borders popover: the uniform border of every frame that has none of its own. */
static GtkWidget *_borders_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(4));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_container_set_border_width(GTK_CONTAINER(grid), DT_PIXEL_APPLY_DPI(10));
  _section_label(grid, 0, _("Borders"));
  toolbar->border_width = gtk_spin_button_new_with_range(0.0, 200.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->border_width, _("Default border width of the frames, in canvas units"));
  g_signal_connect(toolbar->border_width, "value-changed", G_CALLBACK(_border_changed), self);
  _labelled(grid, 1, 0, _("Width"), toolbar->border_width);
  toolbar->border_color = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(toolbar->border_color), TRUE);
  gtk_widget_set_tooltip_text(toolbar->border_color, _("Default border colour of the frames"));
  g_signal_connect(toolbar->border_color, "color-set", G_CALLBACK(_border_changed), self);
  _labelled(grid, 1, 1, _("Colour"), toolbar->border_color);
  toolbar->corner_radius = gtk_spin_button_new_with_range(0.0, 5000.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->corner_radius,
                              _("Default radius of the frames' rounded corners, in canvas units; 0 is square"));
  g_signal_connect(toolbar->corner_radius, "value-changed", G_CALLBACK(_corner_changed), self);
  _labelled(grid, 2, 0, _("Corners"), toolbar->corner_radius);
  GtkWidget *popover = gtk_popover_new(NULL);
  gtk_container_add(GTK_CONTAINER(popover), grid);
  gtk_widget_show_all(grid);
  return popover;
}

/**
 * The texture popover: the four degrees of freedom every paper answers to. Contrast weighs
 * the body of the relief, detail its fine structure, scale sizes its features, grain the
 * dither that finishes it; 1 everywhere is the paper as designed.
 */
static GtkWidget *_texture_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(4));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_container_set_border_width(GTK_CONTAINER(grid), DT_PIXEL_APPLY_DPI(10));
  _section_label(grid, 0, _("Paper texture"));
  toolbar->texture_contrast = _popover_slider(grid, 1, _("Contrast"), 0.05, 4.0, 0.05,
                                              _("The relief's body: the mottle, the tooth, the clouds. 1 is the paper as designed."),
                                              G_CALLBACK(_texture_changed), self);
  toolbar->texture_detail = _popover_slider(grid, 2, _("Detail"), 0.0, 4.0, 0.05,
                                            _("The fine structure: fibres, pores, wrinkles, the mesh's imprint. 0 leaves only the body."),
                                            G_CALLBACK(_texture_changed), self);
  toolbar->texture_scale = _popover_slider(grid, 3, _("Scale"), 0.25, 4.0, 0.05,
                                           _("The size of the features: 2 makes them twice as large. Rebuilds the paper."),
                                           G_CALLBACK(_texture_changed), self);
  toolbar->texture_grain = _popover_slider(grid, 4, _("Grain"), 0.0, 4.0, 0.05,
                                           _("The pixel-level grain that finishes the paper, scaled with the zoom"),
                                           G_CALLBACK(_texture_changed), self);
  GtkWidget *reset = gtk_button_new_with_label(_("Reset"));
  gtk_widget_set_tooltip_text(reset, _("The paper as designed"));
  g_signal_connect(reset, "clicked", G_CALLBACK(_texture_reset), self);
  gtk_grid_attach(GTK_GRID(grid), reset, 3, 5, 1, 1);
  GtkWidget *popover = gtk_popover_new(NULL);
  gtk_container_add(GTK_CONTAINER(popover), grid);
  gtk_widget_show_all(grid);
  return popover;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = g_new0(dt_lib_canvas_toolbar_t, 1);
  self->data = toolbar;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  self->widget = box;
  dt_gui_add_class(box, "dt-canvas-toolbar");

  GtkWidget *canvas_menu = gtk_menu_new();
  _action_item(canvas_menu, _("New"), DT_CANVAS_ACTION_NEW);
  _action_item(canvas_menu, _("Open..."), DT_CANVAS_ACTION_OPEN);
  _action_item(canvas_menu, _("Save"), DT_CANVAS_ACTION_SAVE);
  _action_item(canvas_menu, _("Save as..."), DT_CANVAS_ACTION_SAVE_AS);
  gtk_menu_shell_append(GTK_MENU_SHELL(canvas_menu), gtk_separator_menu_item_new());
  _action_item(canvas_menu, _("Export..."), DT_CANVAS_ACTION_EXPORT);
  _menu_button(box, _("Canvas"), _("New, open, save and export the canvas"), canvas_menu);

  GtkWidget *object_menu = gtk_menu_new();
  _action_item(object_menu, _("Check against the library"), DT_CANVAS_ACTION_SYNC_CHECK);
  _action_item(object_menu, _("Refresh the stale images and the notes"), DT_CANVAS_ACTION_SYNC_REFRESH_STALE);
  _action_item(object_menu, _("Refresh every image"), DT_CANVAS_ACTION_SYNC_REFRESH_ALL);
  _menu_button(box, _("Object"), _("Keep the images and notes in step with the library"), object_menu);

  _popover_button(box, _("Guides"), _("The grid, the page borders and the gutters: what shows and what snaps"),
                  _guides_popover(self));
  _separator(box);

  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Add")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  _button(box, _("Text"), _("Add a text frame at the centre of the view"), DT_CANVAS_ACTION_ADD_TEXT);
  _button(box, _("Notes"), _("Add the .txt notes of the selected images as text frames (of every image when none is selected)"),
          DT_CANVAS_ACTION_ADD_NOTES);
  _button(box, _("Map"), _("Add a map frame at the centre of the view; an image's context menu adds a map of where it was taken"),
          DT_CANVAS_ACTION_ADD_MAP);
  toolbar->connect_toggle = gtk_toggle_button_new_with_label(_("Connector"));
  gtk_widget_set_tooltip_text(toolbar->connect_toggle,
                              _("Draw a connector: click an anchor point on one frame, then on another"));
  g_signal_connect(toolbar->connect_toggle, "toggled", G_CALLBACK(_connect_toggled), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->connect_toggle, FALSE, FALSE, 0);
  _separator(box);

  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Background")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  toolbar->background_style = gtk_combo_box_text_new();
  for(int position = 0; position < dt_canvas_background_count(); position++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->background_style), dt_canvas_background_name(position));
  gtk_widget_set_tooltip_text(toolbar->background_style,
                              _("What the canvas is painted with. Transparent leaves it a hole, shown here as a chequerboard "
                                "and carried out by any export format with an alpha channel."));
  g_signal_connect(toolbar->background_style, "changed", G_CALLBACK(_background_changed), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->background_style, FALSE, FALSE, 0);
  toolbar->background_color = gtk_color_button_new();
  gtk_widget_set_tooltip_text(toolbar->background_color, _("Background colour: the plain colour, or the paper's own"));
  g_signal_connect(toolbar->background_color, "color-set", G_CALLBACK(_background_changed), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->background_color, FALSE, FALSE, 0);
  _popover_button(box, _("Texture"), _("The paper's relief, detail, scale and grain"), _texture_popover(self));
  _separator(box);

  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Frames")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  _popover_button(box, _("Borders"), _("The uniform border of every frame without one of its own"), _borders_popover(self));
  _popover_button(box, _("Shadows"), _("The default shadow of every object without one of its own"), _shadow_popover(self));
  _separator(box);

  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Zoom")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  _button(box, _("Fit"), _("Fit the view to the canvas"), DT_CANVAS_ACTION_ZOOM_FIT);
  _button(box, _("1:1"), _("Zoom to 100%"), DT_CANVAS_ACTION_ZOOM_100);
  _separator(box);

  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Arrange")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  toolbar->layout = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Square grid"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Masonry"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Row"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Column"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->layout), 0);
  gtk_widget_set_tooltip_text(toolbar->layout, _("How to arrange the selected frames, or all of them"));
  gtk_box_pack_start(GTK_BOX(box), toolbar->layout, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Sort by")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  toolbar->sort = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("canvas order"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("filename"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("captured"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("id"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("full path"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->sort), CLAMP(dt_conf_get_int("canvas/layout_sort"), 0, DT_CANVAS_SORT_LAST - 1));
  gtk_widget_set_tooltip_text(toolbar->sort,
                              _("The order the frames are arranged in: the canvas's own, or a key of the images as in the lighttable; frames that are not images follow"));
  g_signal_connect(toolbar->sort, "changed", G_CALLBACK(_sort_changed), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->sort, FALSE, FALSE, 0);
  GtkWidget *arrange = gtk_button_new_with_label(_("Auto"));
  gtk_widget_set_tooltip_text(arrange, _("Arrange the frames in the chosen layout and order"));
  g_signal_connect(arrange, "clicked", G_CALLBACK(_layout_apply), self);
  gtk_box_pack_start(GTK_BOX(box), arrange, FALSE, FALSE, 0);

  gtk_widget_show_all(box);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED,
                                  G_CALLBACK(_canvas_changed), self);
  _refill(self);
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  _refill(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_canvas_changed), self);
  dt_free(self->data);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
