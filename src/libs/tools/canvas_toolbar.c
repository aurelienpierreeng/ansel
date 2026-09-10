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
 * @brief The Canvas atelier's toolbar: file actions, grid, default border, zoom, layouts, sync.
 *
 * @details The toolbar holds no document state. Every button asks the view for an action
 * through `proxy.canvas` (see canvas/canvas_actions.h) and the controls that mirror
 * document settings -- the grid toggles and size, the default border -- are refilled from
 * the document on DT_SIGNAL_CANVAS_CHANGED, with their handlers blocked so a refill never
 * writes back.
 */

#include "canvas/canvas.h"
#include "canvas/canvas_actions.h"
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
  GtkWidget *grid_toggle;
  GtkWidget *snap_toggle;
  GtkWidget *grid_size;
  GtkWidget *border_width;
  GtkWidget *border_color;
  GtkWidget *layout;
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

static void _toggle_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  if(toolbar->refilling) return;
  const dt_canvas_t *canvas = _document();
  if(IS_NULL_PTR(canvas)) return;
  // Only toggle when the document disagrees with the button: a refill already matched them.
  if(GTK_WIDGET(button) == toolbar->grid_toggle
     && gtk_toggle_button_get_active(button) != ((canvas->grid_flags & DT_CANVAS_GRID_VISIBLE) != 0))
    _ask(DT_CANVAS_ACTION_TOGGLE_GRID);
  else if(GTK_WIDGET(button) == toolbar->snap_toggle
          && gtk_toggle_button_get_active(button) != ((canvas->grid_flags & DT_CANVAS_GRID_SNAP) != 0))
    _ask(DT_CANVAS_ACTION_TOGGLE_SNAP);
}

static void _grid_size_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  if(toolbar->refilling) return;
  dt_view_t *view = _canvas_view();
  if(IS_NULL_PTR(view) || IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_grid_size)) return;
  dt_view_manager_get_global()->proxy.canvas.set_grid_size(view, (float)gtk_spin_button_get_value(spin));
}

static void _border_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  if(toolbar->refilling) return;
  dt_view_t *view = _canvas_view();
  if(IS_NULL_PTR(view) || IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_border)) return;
  GdkRGBA rgba;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(toolbar->border_color), &rgba);
  const float color[4] = { (float)rgba.red, (float)rgba.green, (float)rgba.blue, (float)rgba.alpha };
  const float width = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->border_width));
  dt_view_manager_get_global()->proxy.canvas.set_border(view, color, width);
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
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->grid_toggle), (canvas->grid_flags & DT_CANVAS_GRID_VISIBLE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->snap_toggle), (canvas->grid_flags & DT_CANVAS_GRID_SNAP) != 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->grid_size), canvas->grid_size);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->border_width), canvas->border_width);
  GdkRGBA rgba;
  rgba.red = canvas->border_color.red;
  rgba.green = canvas->border_color.green;
  rgba.blue = canvas->border_color.blue;
  rgba.alpha = canvas->border_color.alpha;
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(toolbar->border_color), &rgba);
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

void gui_init(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = g_new0(dt_lib_canvas_toolbar_t, 1);
  self->data = toolbar;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  self->widget = box;
  dt_gui_add_class(box, "dt-canvas-toolbar");

  _button(box, _("New"), _("Start an empty canvas"), DT_CANVAS_ACTION_NEW);
  _button(box, _("Open"), _("Open a saved canvas"), DT_CANVAS_ACTION_OPEN);
  _button(box, _("Save"), _("Save the canvas"), DT_CANVAS_ACTION_SAVE);
  _button(box, _("Save as"), _("Save the canvas under another name"), DT_CANVAS_ACTION_SAVE_AS);
  _button(box, _("PDF"), _("Export the canvas as a colour-managed PDF"), DT_CANVAS_ACTION_EXPORT_PDF);
  _separator(box);
  _button(box, _("Text"), _("Add a text frame at the centre of the view"), DT_CANVAS_ACTION_ADD_TEXT);
  _button(box, _("Notes"), _("Add the .txt notes of the selected images as text frames (of every image when none is selected)"),
          DT_CANVAS_ACTION_ADD_NOTES);
  _separator(box);

  toolbar->grid_toggle = gtk_toggle_button_new_with_label(_("Grid"));
  gtk_widget_set_tooltip_text(toolbar->grid_toggle, _("Show the grid dots"));
  g_signal_connect(toolbar->grid_toggle, "toggled", G_CALLBACK(_toggle_toggled), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->grid_toggle, FALSE, FALSE, 0);
  toolbar->snap_toggle = gtk_toggle_button_new_with_label(_("Snap"));
  gtk_widget_set_tooltip_text(toolbar->snap_toggle, _("Snap frames to the grid when moving or scaling them"));
  g_signal_connect(toolbar->snap_toggle, "toggled", G_CALLBACK(_toggle_toggled), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->snap_toggle, FALSE, FALSE, 0);
  toolbar->grid_size = gtk_spin_button_new_with_range(5.0, 1000.0, 5.0);
  gtk_widget_set_tooltip_text(toolbar->grid_size, _("Grid spacing, in canvas units"));
  g_signal_connect(toolbar->grid_size, "value-changed", G_CALLBACK(_grid_size_changed), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->grid_size, FALSE, FALSE, 0);
  _separator(box);

  GtkWidget *border_label = gtk_label_new(_("Border"));
  gtk_box_pack_start(GTK_BOX(box), border_label, FALSE, FALSE, 0);
  toolbar->border_width = gtk_spin_button_new_with_range(0.0, 200.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->border_width, _("Default border width of the frames, in canvas units"));
  g_signal_connect(toolbar->border_width, "value-changed", G_CALLBACK(_border_changed), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->border_width, FALSE, FALSE, 0);
  toolbar->border_color = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(toolbar->border_color), TRUE);
  gtk_widget_set_tooltip_text(toolbar->border_color, _("Default border colour of the frames"));
  g_signal_connect(toolbar->border_color, "color-set", G_CALLBACK(_border_changed), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->border_color, FALSE, FALSE, 0);
  _separator(box);

  _button(box, _("Fit"), _("Fit the view to the canvas"), DT_CANVAS_ACTION_ZOOM_FIT);
  _button(box, _("1:1"), _("Zoom to 100%"), DT_CANVAS_ACTION_ZOOM_100);
  _separator(box);

  toolbar->layout = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Square grid"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Masonry"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Row"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Column"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->layout), 0);
  gtk_widget_set_tooltip_text(toolbar->layout, _("How to arrange the selected frames, or all of them"));
  gtk_box_pack_start(GTK_BOX(box), toolbar->layout, FALSE, FALSE, 0);
  GtkWidget *arrange = gtk_button_new_with_label(_("Arrange"));
  gtk_widget_set_tooltip_text(arrange, _("Apply the chosen layout"));
  g_signal_connect(arrange, "clicked", G_CALLBACK(_layout_apply), self);
  gtk_box_pack_start(GTK_BOX(box), arrange, FALSE, FALSE, 0);
  _separator(box);

  _button(box, _("Check"), _("Compare every image with the library"), DT_CANVAS_ACTION_SYNC_CHECK);
  _button(box, _("Refresh"), _("Re-render the images whose development changed, and reload text notes"),
          DT_CANVAS_ACTION_SYNC_REFRESH_STALE);
  _button(box, _("Refresh all"), _("Re-render every image from the library"), DT_CANVAS_ACTION_SYNC_REFRESH_ALL);

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
