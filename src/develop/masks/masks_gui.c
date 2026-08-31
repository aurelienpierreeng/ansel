/*
    This file is part of Ansel
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2025-2026 Guillaume Stutin.
    
    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "common/glib_utils.h"
#include "common/hash.h"
#include "common/logging.h"
#include "common/times.h"
#include "develop/blend.h"
#include "develop/blend_gui.h"
#include "develop/develop.h"
#include "develop/dev_pixelpipe.h"
#include "develop/imageop.h"
#include "develop/supervisor.h"
#include "math/math.h"
#include "system/dtpthread.h"
#include "widgets/gdkkeys.h"
#include "widgets/accelerators.h"
#include "control/control.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "develop/geometry/geometry.h"   // dt_geometry_chain_generation()
#include "develop/masks.h"
#include "develop/masks_debug.h"
#include "develop/masks_gui.h"
#include "develop/masks_group.h"
#include "develop/masks/masks_functions.h"
#include "widgets/bauhaus.h"
#include "common/conf.h"
#include "control/signal.h"
#include "develop/imageop_gui.h"
#include "widgets/paint.h"
#include "gui/actions/menu.h"
#include "widgets/draw.h"
#include "gui/application.h"

#include <math.h>
#include <stdlib.h>
#include "widgets/togglebutton.h"

#define DT_MASKS_SHAPE_BUTTON_COUNT 5

typedef struct dt_masks_shape_button_def_t
{
  int index;
  guint flag;
  dt_masks_type_t type;
  const gchar *label;
  const gchar *ctrl_label;
  DTGTKCairoPaintIconFunc paint;
} dt_masks_shape_button_def_t;

typedef struct dt_masks_shape_buttons_data_t
{
  GtkWidget *box;
  GtkWidget *buttons[DT_MASKS_SHAPE_BUTTON_COUNT];
  int types[DT_MASKS_SHAPE_BUTTON_COUNT];
  dt_masks_shape_buttons_config_t config;
} dt_masks_shape_buttons_data_t;

static const dt_masks_shape_button_def_t _masks_shape_button_defs[] = {
  { DT_MASKS_SHAPE_INDEX_GRADIENT, DT_MASKS_SHAPE_BUTTONS_GRADIENT, DT_MASKS_GRADIENT,
    N_("add gradient"), N_("add multiple gradients"), dtgtk_cairo_paint_masks_gradient },
  { DT_MASKS_SHAPE_INDEX_BRUSH, DT_MASKS_SHAPE_BUTTONS_BRUSH, DT_MASKS_BRUSH,
    N_("add brush"), N_("add multiple brush strokes"), dtgtk_cairo_paint_masks_brush },
  { DT_MASKS_SHAPE_INDEX_POLYGON, DT_MASKS_SHAPE_BUTTONS_POLYGON, DT_MASKS_POLYGON,
    N_("add polygon"), N_("add multiple polygons"), dtgtk_cairo_paint_masks_polygon },
  { DT_MASKS_SHAPE_INDEX_ELLIPSE, DT_MASKS_SHAPE_BUTTONS_ELLIPSE, DT_MASKS_ELLIPSE,
    N_("add ellipse"), N_("add multiple ellipses"), dtgtk_cairo_paint_masks_ellipse },
  { DT_MASKS_SHAPE_INDEX_CIRCLE, DT_MASKS_SHAPE_BUTTONS_CIRCLE, DT_MASKS_CIRCLE,
    N_("add circle"), N_("add multiple circles"), dtgtk_cairo_paint_masks_circle },
};

static void _masks_shape_buttons_deactivate(GtkWidget *active_button, dt_masks_shape_buttons_data_t *data)
{
  if(IS_NULL_PTR(data)) return;

  // Walk all buttons in this group so any caller can reset every masks shape toolbar through the shared signal.
  for(int i = 0; i < DT_MASKS_SHAPE_BUTTON_COUNT; i++)
  {
    GtkWidget *button = data->buttons[i];
    if(GTK_IS_TOGGLE_BUTTON(button) && button != active_button)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
  }
}

static void _masks_shape_buttons_deactivate_signal(gpointer instance, GtkWidget *active_button,
                                                   dt_masks_shape_buttons_data_t *data)
{
  _masks_shape_buttons_deactivate(active_button, data);
}

void dt_masks_shape_buttons_deactivate_all(GtkWidget *active_button)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_SHAPE_BUTTONS_DEACTIVATE, active_button);
}

static int _masks_shape_button_index(const dt_masks_shape_buttons_data_t *data, GtkWidget *button)
{
  if(IS_NULL_PTR(data)) return -1;

  // Search the stored button pointers because callers may keep their own storage arrays.
  for(int i = 0; i < DT_MASKS_SHAPE_BUTTON_COUNT; i++)
    if(data->buttons[i] == button) return i;

  return -1;
}

static gboolean _masks_shape_button_is_current_creation(dt_develop_t *dev,
                                                        const dt_masks_shape_buttons_data_t *data,
                                                        const int button_index)
{
  dt_masks_form_gui_t *mask_gui = dev->form_gui;
  dt_masks_form_t *visible_form = dt_masks_get_visible_form(dev);

  return !IS_NULL_PTR(mask_gui) && mask_gui->creation
         && mask_gui->creation_module == data->config.creation_module
         && !IS_NULL_PTR(visible_form)
         && (visible_form->type & data->types[button_index]);
}

static gboolean _masks_shape_button_pressed(GtkWidget *button, GdkEventButton *event, gpointer user_data)
{
  if(dt_gui_widgets_suppressed() || event->button != GDK_BUTTON_PRIMARY) return TRUE;

  dt_masks_shape_buttons_data_t *data =
      (dt_masks_shape_buttons_data_t *)g_object_get_data(G_OBJECT(button), "dt-masks-shape-buttons-data");
  const int button_index = _masks_shape_button_index(data, button);
  if(button_index < 0) return FALSE;

  dt_masks_type_t type = data->types[button_index];
  dt_iop_module_t *module = data->config.creation_module;
  dt_develop_t *dev = !IS_NULL_PTR(module) ? module->dev : data->config.dev;
  if(IS_NULL_PTR(dev)) return FALSE;
  dt_masks_form_gui_t *mask_gui = dev->form_gui;

  if(_masks_shape_button_is_current_creation(dev, data, button_index))
  {
    dt_masks_shape_buttons_deactivate_all(NULL);
    dt_masks_form_exit_creation(module, mask_gui);
    if(data->config.exited) data->config.exited(button, module, type, data->config.user_data);
    dt_control_queue_redraw_center();
    return TRUE;
  }

  if(data->config.can_start && !data->config.can_start(button, module, type, data->config.user_data))
  {
    dt_masks_shape_buttons_deactivate_all(NULL);
    dt_control_queue_redraw_center();
    return TRUE;
  }

  if(data->config.form_type) type = data->config.form_type(module, type, data->config.user_data);

  dt_masks_shape_buttons_deactivate_all(button);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);

  if(dt_masks_creation_mode_enter(dev, module, type))
  {
    if(data->config.started)
    {
      data->config.started(button, module, type, data->config.user_data);
      // Force focus back to the drawing area after creation mode enabling
      gtk_widget_grab_focus(dt_gui_center_widget());
    }
  }
  else
  {
    dt_masks_shape_buttons_deactivate_all(NULL);
  }

  dt_control_queue_redraw_center();
  return TRUE;
}

static void _masks_shape_buttons_destroy(GtkWidget *widget, dt_masks_shape_buttons_data_t *data)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_masks_shape_buttons_deactivate_signal), data);
  dt_free(data);
}

/**
 * @brief Build a synchronized toolbar for creating masks shapes.
 *
 * The buttons all use the same creation callback and listen to a process-wide
 * deactivation signal. This keeps multiple mask toolbars, such as blending,
 * retouch and the shape manager, from showing stale active buttons after
 * another toolbar starts or exits a shape creation.
 */
GtkWidget *dt_masks_shape_buttons_create(const dt_masks_shape_buttons_config_t *config)
{
  if(IS_NULL_PTR(config)) return NULL;

  dt_masks_shape_buttons_data_t *data = calloc(1, sizeof(dt_masks_shape_buttons_data_t));
  if(IS_NULL_PTR(data)) return NULL;

  data->config = *config;
  data->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_halign(data->box, GTK_ALIGN_END);
  gtk_widget_set_valign(data->box, GTK_ALIGN_CENTER);

  const char *action_section = config->action_section ? config->action_section : N_("shapes");
  const size_t button_defs_count = sizeof(_masks_shape_button_defs) / sizeof(_masks_shape_button_defs[0]);

  // Create buttons in the same visible order used by the module-local toolbars.
  for(size_t i = 0; i < button_defs_count; i++)
  {
    const dt_masks_shape_button_def_t *def = &_masks_shape_button_defs[i];
    if(!(config->flags & def->flag)) continue;

    GtkWidget *button = NULL;
    if(config->owner_module)
    {
      const gboolean register_button = (config->register_flags & def->flag);
      if(register_button)
      {
        button = dt_iop_togglebutton_new(config->owner_module, action_section, def->label, def->ctrl_label,
                                         G_CALLBACK(_masks_shape_button_pressed), config->local,
                                         0, 0, def->paint, data->box);
      }
      else
      {
        button = dt_iop_togglebutton_new_no_register(config->owner_module, action_section, def->label, def->ctrl_label,
                                                     G_CALLBACK(_masks_shape_button_pressed), config->local,
                                                     0, 0, def->paint, data->box);
      }
    }
    else
    {
      button = dtgtk_togglebutton_new(def->paint, 0, NULL);
      gtk_widget_set_tooltip_text(button, _(def->label));
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
      gtk_box_pack_end(GTK_BOX(data->box), button, FALSE, FALSE, 0);
      g_signal_connect(G_OBJECT(button), "button-press-event", G_CALLBACK(_masks_shape_button_pressed), NULL);
    }

    gtk_widget_set_can_focus(button, FALSE);
    g_object_set_data(G_OBJECT(button), "dt-masks-shape-buttons-data", data);

    data->buttons[def->index] = button;
    data->types[def->index] = def->type;
    if(config->buttons) config->buttons[def->index] = button;
    if(config->types) config->types[def->index] = def->type;
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_MASK_SHAPE_BUTTONS_DEACTIVATE,
                                  G_CALLBACK(_masks_shape_buttons_deactivate_signal), data);
  g_signal_connect(G_OBJECT(data->box), "destroy", G_CALLBACK(_masks_shape_buttons_destroy), data);

  return data->box;
}

typedef struct dt_masks_gui_interaction_slider_t
{
  /* The row this slider drives, named by IDENTITY and never held as a pointer. A slider outlives
   * many mutations of the group it points into: it commits history on every step, each commit
   * re-snapshots dev->forms, and the next step's copy-on-write then clones the group -- so a
   * cached dt_masks_form_group_t* would, from the second step on, address the abandoned copy
   * while the snapshots it was supposed to leave frozen kept the edits. */
  dt_develop_t *dev;
  int group_id;
  int formid;
  dt_masks_form_gui_t *gui;
  dt_iop_module_t *module;
  dt_masks_interaction_t interaction;
  dt_masks_increment_t increment;
  float last_value;
  GtkWidget *slider;
} dt_masks_gui_interaction_slider_t;

// Push the new value to history (so the pipeline re-renders) and refresh the mask
// treeviews (opacity text, etc.).
//
// This is called from the slider "value-changed" handler, which fires on every step of a
// drag. That is fine: the history commit batches the pipeline resync it triggers, so
// transient values do not flood the pipeline with renders, yet the image updates without
// waiting for the context menu to be closed.
static void _masks_gui_interaction_commit(dt_masks_gui_interaction_slider_t *data)
{
  if(IS_NULL_PTR(data) || IS_NULL_PTR(data->gui)) return;

  dt_dev_add_history_item(data->gui->dev, data->module, TRUE, TRUE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED,
                                data->formid, data->group_id, DT_MASKS_EVENT_UPDATE);
}

static void _masks_gui_interaction_apply_value(dt_masks_gui_interaction_slider_t *data, float value)
{
  if(IS_NULL_PTR(data) || IS_NULL_PTR(data->dev)) return;

  if(data->increment == DT_MASKS_INCREMENT_ABSOLUTE) // aka opacity
  {
    dt_masks_form_set_interaction_value(data->dev, data->group_id, data->formid, data->interaction, value,
                                        data->increment, 1, data->gui, data->module);
    data->last_value = value;
    _masks_gui_interaction_commit(data);
    return;
  }

  const float delta = value - data->last_value;
  if(fabsf(delta) < 1e-6f) return;

  // Slider value is a log2 scale factor in [-3;3], so apply the delta in log space.
  const float scale = exp2f(delta);
  dt_masks_form_set_interaction_value(data->dev, data->group_id, data->formid, data->interaction, scale,
                                      DT_MASKS_INCREMENT_SCALE, 1.f, data->gui, data->module);
  data->last_value = value;
  _masks_gui_interaction_commit(data);
}

static void _masks_gui_menu_item_block_activate(GtkWidget *widget, gpointer user_data)
{
  g_signal_stop_emission_by_name(widget, "activate");
}

static gboolean _masks_gui_menu_item_forward_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_masks_gui_interaction_slider_t *data = (dt_masks_gui_interaction_slider_t *)user_data;
  if(IS_NULL_PTR(data) || !data->slider) return FALSE;

  GdkEvent *copy = gdk_event_copy(event);
  if(IS_NULL_PTR(copy)) return FALSE;

  double x = 0.0, y = 0.0;
  gboolean has_coords = FALSE;
  switch(copy->type)
  {
    case GDK_BUTTON_PRESS:
    case GDK_2BUTTON_PRESS:
    case GDK_3BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
      x = copy->button.x;
      y = copy->button.y;
      has_coords = TRUE;
      break;
    case GDK_MOTION_NOTIFY:
      x = copy->motion.x;
      y = copy->motion.y;
      has_coords = TRUE;
      break;
    case GDK_SCROLL:
      x = copy->scroll.x;
      y = copy->scroll.y;
      has_coords = TRUE;
      break;
    default:
      break;
  }

  if(has_coords)
  {
    int sx = 0, sy = 0;
    if(gtk_widget_translate_coordinates(widget, data->slider, (int)x, (int)y, &sx, &sy))
    {
      switch(copy->type)
      {
        case GDK_BUTTON_PRESS:
        case GDK_2BUTTON_PRESS:
        case GDK_3BUTTON_PRESS:
        case GDK_BUTTON_RELEASE:
          copy->button.x = sx;
          copy->button.y = sy;
          break;
        case GDK_MOTION_NOTIFY:
          copy->motion.x = sx;
          copy->motion.y = sy;
          break;
        case GDK_SCROLL:
          copy->scroll.x = sx;
          copy->scroll.y = sy;
          break;
        default:
          break;
      }
    }
  }

  GdkWindow *slider_window = gtk_widget_get_window(data->slider);
  if(slider_window)
  {
    if(copy->any.window) g_object_unref(copy->any.window);
    copy->any.window = g_object_ref(slider_window);
    copy->any.send_event = TRUE;
  }

  gtk_widget_event(data->slider, copy);
  gdk_event_free(copy);
  return TRUE;
}

static void _masks_gui_interaction_slider_changed(GtkWidget *widget, gpointer user_data)
{
  dt_masks_gui_interaction_slider_t *data = (dt_masks_gui_interaction_slider_t *)user_data;
  if(IS_NULL_PTR(data) || IS_NULL_PTR(data->dev)) return;

  _masks_gui_interaction_apply_value(data, dt_bauhaus_slider_get(widget));
}

GtkWidget *dt_masks_gui_add_interaction_slider(GtkWidget *menu, const char *label, dt_develop_t *dev,
                                               const int group_id, const int formid,
                                               dt_masks_interaction_t interaction, dt_masks_increment_t increment,
                                               float min, float max, float step, float value, int digits,
                                               const char *format, float factor,
                                               dt_masks_form_gui_t *gui, dt_iop_module_t *module)
{
  GtkWidget *menu_item = gtk_menu_item_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);

  gtk_widget_set_can_focus(menu_item, FALSE);
  g_signal_connect(G_OBJECT(menu_item), "activate",
                   G_CALLBACK(_masks_gui_menu_item_block_activate), NULL);
  gtk_widget_add_events(menu_item, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                   | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);

  GtkWidget *slider = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), module ? DT_GUI_MODULE(module) : NULL,
                                                       min, max, step, value, digits);
  dt_bauhaus_widget_set_label(slider, label);
  dt_bauhaus_slider_set_digits(slider, digits);
  if(format && format[0] != '\0') dt_bauhaus_slider_set_format(slider, format);
  if(factor != 1.0f) dt_bauhaus_slider_set_factor(slider, factor);
  dt_bauhaus_slider_set(slider, value);
  DT_BAUHAUS_WIDGET(slider)->expand = TRUE;
  gtk_widget_set_hexpand(slider, TRUE);
  gtk_widget_set_halign(slider, GTK_ALIGN_FILL);
  gtk_widget_set_valign(slider, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(slider, DT_PIXEL_APPLY_DPI(220), DT_PIXEL_APPLY_DPI(28));
  gtk_widget_set_can_focus(slider, TRUE);

  dt_masks_gui_interaction_slider_t *data = g_malloc0(sizeof(dt_masks_gui_interaction_slider_t));
  data->dev = dev;
  data->group_id = group_id;
  data->formid = formid;
  data->gui = gui;
  data->module = module;
  data->interaction = interaction;
  data->increment = increment;
  data->last_value = value;
  data->slider = slider;
  g_signal_connect_data(G_OBJECT(slider), "value-changed",
                        G_CALLBACK(_masks_gui_interaction_slider_changed),
                        data, (GClosureNotify)g_free, 0);
  g_signal_connect(G_OBJECT(menu_item), "button-press-event",
                   G_CALLBACK(_masks_gui_menu_item_forward_event), data);
  g_signal_connect(G_OBJECT(menu_item), "button-release-event",
                   G_CALLBACK(_masks_gui_menu_item_forward_event), data);
  g_signal_connect(G_OBJECT(menu_item), "motion-notify-event",
                   G_CALLBACK(_masks_gui_menu_item_forward_event), data);
  g_signal_connect(G_OBJECT(menu_item), "scroll-event",
                   G_CALLBACK(_masks_gui_menu_item_forward_event), data);

  gtk_box_pack_start(GTK_BOX(box), slider, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(menu_item), box);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

  return menu_item;
}

int dt_masks_gui_confirm_delete_form_dialog(const char *form_name)
{
  if(IS_NULL_PTR(dt_gui_get_global()) || IS_NULL_PTR(dt_gui_get_ui())) return GTK_RESPONSE_NO;

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(dt_gui_main_window()),
      GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Delete the shape '%s' ?"), form_name);
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "'%s' %s\n\n%s", form_name,
      _("will no longer be used."),
      _("Do you want to permanently delete it, or keep it unused for potential reuse?"));

  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Delete shape"), GTK_RESPONSE_YES);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Keep unused shape"), GTK_RESPONSE_NO);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);

  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  return response;
}

gboolean dt_masks_gui_confirm_permanent_delete(const char *form_name)
{
  if(!dt_conf_get_bool("ask_before_delete_mask_shape")) return TRUE;
  if(IS_NULL_PTR(dt_gui_get_global()) || IS_NULL_PTR(dt_gui_get_ui())) return TRUE;

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(dt_gui_main_window()),
      GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
      _("Permanently delete the shape '%s' ?"), form_name);
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      _("It will be removed from every mask using it and from the list of available shapes."));

  GtkWidget *message_area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
  GtkWidget *ask_check = gtk_check_button_new_with_label(_("Always ask"));
  gtk_widget_set_tooltip_text(ask_check,
      _("when unchecked, mask shapes will be deleted silently from now on without this confirmation.\n"
        "you can turn it back on from preferences."));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ask_check), TRUE);
  gtk_box_pack_start(GTK_BOX(message_area), ask_check, FALSE, FALSE, 6);
  gtk_widget_show(ask_check);

  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  dt_conf_set_bool("ask_before_delete_mask_shape", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ask_check)));
  gtk_widget_destroy(dialog);

  return response == GTK_RESPONSE_YES;
}

// Resolves the group entry selected in gui to (module, sel, parentid, formid). Returns FALSE
// if nothing usable is selected, in which case *module/*sel are left untouched.
static gboolean _masks_gui_resolve_selected_shape(dt_masks_form_gui_t *gui, dt_iop_module_t **module,
                                                   dt_masks_form_t **sel, int *parentid, int *formid)
{
  if(IS_NULL_PTR(gui) || gui->group_selected < 0) return FALSE;
  dt_masks_form_t *forms = dt_masks_get_visible_form(gui->dev);
  if(IS_NULL_PTR(forms)) return FALSE;

  dt_masks_form_group_t *fpt = dt_masks_form_get_selected_group(forms, gui);
  if(IS_NULL_PTR(fpt)) return FALSE;
  dt_iop_module_t *mod = gui->dev->gui_module;
  if(IS_NULL_PTR(mod)) return FALSE;
  dt_masks_form_t *form = dt_masks_get_from_id(gui->dev, fpt->formid);
  if(IS_NULL_PTR(form)) return FALSE;

  *module = mod;
  *sel = form;
  *parentid = fpt->parentid;
  *formid = fpt->formid;
  return TRUE;
}

// "Remove shape from mask": detach from the current group only, keep the form for reuse.
static void _masks_gui_remove_from_group_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  dt_iop_module_t *module = NULL;
  dt_masks_form_t *sel = NULL;
  int parentid = 0, formid = 0;
  if(!_masks_gui_resolve_selected_shape(gui, &module, &sel, &parentid, &formid)) return;

  dt_masks_remove_shape_from_group(module, sel, parentid, gui, formid);
}

// "Delete shape": permanently delete, gated by dt_masks_gui_confirm_permanent_delete().
static void _masks_gui_full_delete_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  dt_iop_module_t *module = NULL;
  dt_masks_form_t *sel = NULL;
  int parentid = 0, formid = 0;
  if(!_masks_gui_resolve_selected_shape(gui, &module, &sel, &parentid, &formid)) return;

  dt_masks_delete_shape(module, sel, parentid, gui, formid);
}

void _masks_gui_delete_node_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  if(IS_NULL_PTR(gui)) return;
  dt_masks_form_t *forms = dt_masks_get_visible_form(gui->dev);
  if(IS_NULL_PTR(forms)) return;

  dt_iop_module_t *module = gui->dev->gui_module;
  if(IS_NULL_PTR(module)) return;

  if(gui->creation)
  {
    // Minimum points to create a polygon
    if(gui->node_dragging < 1)
    {
      dt_masks_form_exit_creation(module, gui);
      return;
    }
    dt_masks_form_t *sel = dt_masks_get_visible_form(gui->dev);
    if(sel)
      dt_masks_remove_node(module, sel, 0, gui, 0, gui->node_dragging);
    gui->node_dragging -= 1;
  }
  else if(gui->group_selected >= 0)
  {
    // Delete shape from current group

    dt_masks_form_group_t *fpt = dt_masks_form_get_selected_group(forms, gui);
    if(IS_NULL_PTR(fpt)) return;
    dt_masks_form_t *sel = dt_masks_get_from_id(gui->dev, fpt->formid);
    if(sel)
      dt_masks_remove_node(module, sel, fpt->parentid, gui, gui->group_selected, gui->node_hovered);

    dt_dev_add_history_item(gui->dev, module, TRUE, TRUE);
  }
}

static void _masks_gui_exit_creation_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  if(IS_NULL_PTR(gui)) return;
  dt_iop_module_t *module = gui->dev->gui_module;
  dt_masks_form_exit_creation(module, gui);
}

static void _masks_move_up_down_callback(gpointer user_data, const int up)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  if(IS_NULL_PTR(gui)) return;
  if(gui->group_selected < 0) return;

  dt_iop_module_t *module = gui->dev->gui_module;
  if(IS_NULL_PTR(module)) return;

  dt_masks_form_t *forms = dt_masks_get_visible_form(gui->dev);
  if(IS_NULL_PTR(forms)) return;
  dt_masks_form_group_t *fpt = dt_masks_form_get_selected_group(forms, gui);
  if(IS_NULL_PTR(fpt)) return;
  dt_masks_form_t *grp = dt_masks_get_from_id(gui->dev, fpt->parentid);
  if(IS_NULL_PTR(grp) || !(grp->type & DT_MASKS_GROUP)) return;
  grp = dt_masks_cow_touch(gui->dev, grp);

  dt_masks_form_move(grp, fpt->formid, up);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, fpt->formid, fpt->parentid, DT_MASKS_EVENT_CHANGE);
}

static void _masks_moveup_callback(GtkWidget *menu, gpointer user_data)
{
  _masks_move_up_down_callback(user_data, 0);
}

static void _masks_movedown_callback(GtkWidget *menu, gpointer user_data)
{
  _masks_move_up_down_callback(user_data, 1);
}

/** Contextual menu */

static void _masks_operation_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  if(IS_NULL_PTR(gui) || IS_NULL_PTR(menu)) return;

  const guint form_pos = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(menu), "form_pos"));
  const dt_masks_state_t state_op = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu), "state_op"));
  // Advert the user if it will have no effect
  if(form_pos == 0 && (state_op & DT_MASKS_STATE_IS_COMBINE_OP) != 0)
  {
    dt_control_log(_("Applying a boolean operation has no effect on the first shape of a group.\n"
         "Move it to at least the 2nd position if you need to use boolean operations"));
  }

  dt_masks_form_group_t *form_op = (dt_masks_form_group_t *)g_object_get_data(G_OBJECT(menu), "op_form");
  if(IS_NULL_PTR(form_op)) return;

  dt_masks_group_entry_apply_operation(form_op, state_op);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, form_op->formid, form_op->parentid, DT_MASKS_EVENT_UPDATE);
}

#define masks_gtk_menu_item_new_bold(label, selected, state, icon)                                        \
{                                                                                                         \
  gchar *op_label = g_strdup(label);                                                                      \
  menu_item = ctx_gtk_check_menu_item_new_with_markup_and_pixbuf(op_label, icon,                          \
                                                                    sub_menu,                             \
                                                                    _masks_operation_callback, gui,       \
                                                                    (selected != 0),                      \
                                                                    ((state) == DT_MASKS_STATE_INVERSE)); \
  dt_free(op_label);                                                                                       \
  op_label = NULL;                                                                                        \
  g_object_set_data(G_OBJECT(menu_item), "state_op", GINT_TO_POINTER(state));                             \
  g_object_set_data(G_OBJECT(menu_item), "op_form", op_form);                                             \
  g_object_set_data(G_OBJECT(menu_item), "form_pos", GINT_TO_POINTER(form_pos));                          \
}


// Shared by the darkroom canvas context menu (dt_masks_create_menu) and the blend module's
// own shape-list context menus (develop/blend_gui.c), so both offer the same shape parameters.
void dt_masks_gui_populate_interaction_sliders(GtkWidget *menu, dt_develop_t *dev, dt_masks_form_t *form,
                                               const int group_id,
                                               dt_masks_form_gui_t *gui, dt_iop_module_t *module)
{
  if(IS_NULL_PTR(menu) || IS_NULL_PTR(form) || IS_NULL_PTR(dev)) return;

  /* The row has to exist -- opacity is read from it, and every slider writes back to it -- but
   * asking by identity means no caller has to resolve one and hand it over, and none of them has
   * to copy-on-write the group merely to open a menu. */
  if(dt_masks_group_get_member(dev, group_id, form->formid, NULL) != DT_MASKS_OK) return;

  const float opacity = dt_masks_form_get_interaction_value(dev, group_id, form->formid, DT_MASKS_INTERACTION_OPACITY);

  if(form->type & DT_MASKS_GRADIENT)
  {
    // For gradients, DT_MASKS_INTERACTION_FADING is the shape curvature -- expose it
    // under its actual name.
    const float curvature = dt_masks_form_get_interaction_value(dev, group_id, form->formid, DT_MASKS_INTERACTION_FADING);
    const float fade = dt_masks_form_get_interaction_value(dev, group_id, form->formid, DT_MASKS_INTERACTION_SIZE);
    float rotation = dt_masks_form_get_interaction_value(dev, group_id, form->formid, DT_MASKS_INTERACTION_ROTATION);
    if(!isfinite(rotation)) rotation = 0.0f;
    if(rotation > 180.0f) rotation -= 360.0f;

    dt_masks_gui_add_interaction_slider(menu, _("Curvature"), dev, group_id, form->formid, DT_MASKS_INTERACTION_FADING,
                                      DT_MASKS_INCREMENT_ABSOLUTE, -2.0f, 2.0f, 0.01f,
                                      isfinite(curvature) ? curvature : 0.0f, 3, "%", 50.0f,
                                      gui, module);
    dt_masks_gui_add_interaction_slider(menu, _("Size"), dev, group_id, form->formid, DT_MASKS_INTERACTION_SIZE,
                                      DT_MASKS_INCREMENT_ABSOLUTE, 0.0f, 1.0f, 0.001f,
                                      isfinite(fade) ? fade : 1.0f, 3, "%", 100.0f,
                                      gui, module);
    dt_masks_gui_add_interaction_slider(menu, _("Rotation"), dev, group_id, form->formid, DT_MASKS_INTERACTION_ROTATION,
                                      DT_MASKS_INCREMENT_ABSOLUTE, -180.0f, 180.0f, 1.0f,
                                      rotation, 1, "\302\260", 1.0f,
                                      gui, module);
  }
  else
  {
    const float fading = dt_masks_form_get_interaction_value(dev, group_id, form->formid, DT_MASKS_INTERACTION_FADING);

    dt_masks_gui_add_interaction_slider(menu, _("Size"), dev, group_id, form->formid, DT_MASKS_INTERACTION_SIZE,
                                      DT_MASKS_INCREMENT_SCALE, -4.f, 4.0f, 0.01f, 0.0f, 2, "x", 1.0f,
                                      gui, module);
    dt_masks_gui_add_interaction_slider(menu, _("Fading"), dev, group_id, form->formid, DT_MASKS_INTERACTION_FADING,
                                      DT_MASKS_INCREMENT_ABSOLUTE, 0.f, 1.0f, 0.01f,
                                      isfinite(fading) ? fading : 1.0f, 3, "%", 100.0f,
                                      gui, module);

    if(form->type & DT_MASKS_ELLIPSE)
    {
      float rotation = dt_masks_form_get_interaction_value(dev, group_id, form->formid, DT_MASKS_INTERACTION_ROTATION);
      if(!isfinite(rotation)) rotation = 0.0f;
      if(rotation > 180.0f) rotation -= 360.0f;

      dt_masks_gui_add_interaction_slider(menu, _("Rotation"), dev, group_id, form->formid, DT_MASKS_INTERACTION_ROTATION,
                                        DT_MASKS_INCREMENT_ABSOLUTE, -180.0f, 180.0f, 1.0f,
                                        rotation, 1, "\302\260", 1.0f,
                                        gui, module);
    }
  }

  dt_masks_gui_add_interaction_slider(menu, _("Opacity"), dev, group_id, form->formid, DT_MASKS_INTERACTION_OPACITY,
                                    DT_MASKS_INCREMENT_ABSOLUTE, 0.0f, 1.0f, 0.01f,
                                    isfinite(opacity) ? opacity : 1.0f, 3, "%", 100.0f,
                                    gui, module);
}

GtkWidget *dt_masks_create_menu(dt_masks_form_gui_t *gui, dt_masks_form_t *form, const dt_masks_form_group_t *formgroup,
                                const float pzx, const float pzy)
{
  assert(gui);
  assert(form);
  // Always re-create the menu when we show it because we don't bother updating info during the lifetime of the mask
  GtkWidget *menu = gtk_menu_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(menu), "dt-masks-context-menu");

  // Create an array of icons for the operations
  const int bs2 = DT_PIXEL_APPLY_DPI(13);
  GdkPixbuf *op_icon[DT_MASKS_STATE_EXCLUSION + 1] = { 0 };
  int width = bs2 * 2;
  op_icon[DT_MASKS_STATE_INVERSE] = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_inverse, width, bs2);
  op_icon[DT_MASKS_STATE_UNION] = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_union, width, bs2);
  op_icon[DT_MASKS_STATE_INTERSECTION] = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_intersection, width, bs2);
  op_icon[DT_MASKS_STATE_DIFFERENCE] = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_difference, width, bs2);
  op_icon[DT_MASKS_STATE_EXCLUSION] = dt_draw_get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_exclusion, width, bs2);

  // Get the current group to apply operations on it if needed
  dt_masks_form_group_t *op_form = NULL;
  dt_masks_form_t *grp = formgroup ? dt_masks_get_from_id(gui->dev, formgroup->parentid) : NULL;
  if(grp && (grp->type & DT_MASKS_GROUP))
    op_form = dt_masks_form_group_from_parentid(gui->dev, grp->formid, form->formid);
  if(IS_NULL_PTR(op_form) && !gui->creation)
  {
    for(size_t k = 0; k < G_N_ELEMENTS(op_icon); k++)
      g_clear_object(&op_icon[k]);
    gtk_widget_destroy(menu);
    return NULL;
  }

  // Find the position of the current form in the group
  guint form_pos = 0;
  gboolean form_found = FALSE;
  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
    {
      dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
      if(fpt->formid == form->formid)
      {
        form_found = TRUE;
        break;
      }
      form_pos++;
    }
  }

  // Get the number of shapes in the group
  guint list_length = (form_found && grp) ? g_list_length(grp->points) : 0;


  // Title
  gchar *form_name = NULL;
  if(form->name[0])
    form_name = g_strdup(form->name);
  else if(gui->creation)
  {
    // if no name, we are probably creating a new form, we create one based on the type
    form_name = g_strdup(_("New "));
    switch (form->type)
    {
      case DT_MASKS_CIRCLE:
        form_name = g_strconcat(form_name, _("circle"), NULL);
        break;
      case DT_MASKS_ELLIPSE:
        form_name = g_strconcat(form_name, _("ellipse"), NULL);
        break;
      case DT_MASKS_POLYGON:
        form_name = g_strconcat(form_name, _("polygon"), NULL);
        break;
      case DT_MASKS_BRUSH:
        form_name = g_strconcat(form_name, _("brush"), NULL);
        break;
      case DT_MASKS_GRADIENT:
        form_name = g_strconcat(form_name, _("gradient"), NULL);
        break;
      case DT_MASKS_GROUP:
        form_name = g_strconcat(form_name, _("mask"), NULL);
        break;
      default:
        dt_free(form_name); // Erase the "New " prefix
        form_name = g_strdup(_("Unknown shape"));
        break;
    }
  }

  // Create the main label string
  gchar *item_str = NULL;
  if(gui->node_hovered >= 0 || gui->seg_hovered >= 0)
  {
    const int item_index = (gui->node_hovered >= 0) ? gui->node_hovered : gui->seg_hovered;
    item_str = g_strdup_printf("%s %d - ", gui->node_hovered >= 0 ? _("Node") : _("Segment"), item_index);
  }
  else
    item_str = g_strdup("");

  // Create an assembled image if we have an inverse state to show
  const dt_masks_state_t state = IS_NULL_PTR(op_form) ? 0 : op_form->state & DT_MASKS_STATE_IS_COMBINE_OP;
  const gboolean has_inverse = !IS_NULL_PTR(op_form) && (op_form->state & DT_MASKS_STATE_INVERSE) != 0;
  GdkPixbuf *icon = (state <= DT_MASKS_STATE_EXCLUSION) ? op_icon[state] : NULL;
  GdkPixbuf *composed_icon = NULL;
  if(has_inverse && op_icon[DT_MASKS_STATE_INVERSE])
  {
    if(icon)
    {
      const int base_w = gdk_pixbuf_get_width(icon);
      const int base_h = gdk_pixbuf_get_height(icon);
      const int inv_w = gdk_pixbuf_get_width(op_icon[DT_MASKS_STATE_INVERSE]);
      const int inv_h = gdk_pixbuf_get_height(op_icon[DT_MASKS_STATE_INVERSE]);
      const int out_w = base_w + inv_w;
      const int out_h = MAX(base_h, inv_h);

      composed_icon = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, out_w, out_h);
      if(composed_icon)
      {
        gdk_pixbuf_fill(composed_icon, 0x00000000);
        gdk_pixbuf_copy_area(icon, 0, 0, base_w, base_h, composed_icon, 0, 0);
        gdk_pixbuf_copy_area(op_icon[DT_MASKS_STATE_INVERSE], 0, 0, inv_w, inv_h, composed_icon, base_w, 0);
        icon = composed_icon;
      }
    }
    else
      icon = op_icon[DT_MASKS_STATE_INVERSE];
  }

  const gboolean draw_icon = !IS_NULL_PTR(op_form) && form_pos > 0;
  gchar *title = g_strdup_printf("<b><big>%s%s</big></b>", item_str, form_name);
  GtkWidget *menu_item = ctx_gtk_menu_item_new_with_markup_and_pixbuf(title, (draw_icon) ? icon : NULL, menu, NULL, gui);
  gtk_widget_set_sensitive(menu_item, FALSE);
  dt_free(item_str);
  dt_free(title);
  dt_free(form_name);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // Common menu items
  if(!gui->creation && (gui->form_selected || gui->node_selected) && op_form)
  {
    dt_masks_gui_populate_interaction_sliders(menu, gui->dev, form, grp->formid, gui, gui->dev->gui_module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  }

  // Shape specific menu items
  if(!IS_NULL_PTR(form) && form->functions && form->functions->populate_context_menu)
    if(form->functions->populate_context_menu(menu, form, gui, pzx, pzy))
    {
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }


  /* Module specific */
  {
    dt_iop_module_t *module = gui->dev->gui_module;
    if(!IS_NULL_PTR(module) && module->populate_masks_context_menu)
      if(module->populate_masks_context_menu(module, menu, form->formid, pzx, pzy))
      {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
      }
  }

  /*  Operation */

  if(!gui->creation && !(form->type & DT_MASKS_IS_RETOUCHE) && (op_form) && !gui->node_selected)
  {
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Operation"), menu, NULL, gui);
    GtkWidget *sub_menu = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sub_menu), "dt-masks-context-menu");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item), sub_menu);

    masks_gtk_menu_item_new_bold(_("Invert"), (op_form->state & DT_MASKS_STATE_INVERSE), DT_MASKS_STATE_INVERSE,
                                 op_icon[DT_MASKS_STATE_INVERSE]);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub_menu), gtk_separator_menu_item_new());
    masks_gtk_menu_item_new_bold(_("Union"), (op_form->state & DT_MASKS_STATE_UNION), DT_MASKS_STATE_UNION,
                                 op_icon[DT_MASKS_STATE_UNION]);
    masks_gtk_menu_item_new_bold(_("Intersection"), (op_form->state & DT_MASKS_STATE_INTERSECTION), DT_MASKS_STATE_INTERSECTION,
                                 op_icon[DT_MASKS_STATE_INTERSECTION]);
    masks_gtk_menu_item_new_bold(_("Difference"), (op_form->state & DT_MASKS_STATE_DIFFERENCE), DT_MASKS_STATE_DIFFERENCE,
                                 op_icon[DT_MASKS_STATE_DIFFERENCE]);
    masks_gtk_menu_item_new_bold(_("Exclusion"), (op_form->state & DT_MASKS_STATE_EXCLUSION), DT_MASKS_STATE_EXCLUSION,
                                 op_icon[DT_MASKS_STATE_EXCLUSION]);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  }

  if(!gui->creation && gui->form_selected)
  {
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Move up"), menu, _masks_moveup_callback, gui);
    gtk_widget_set_sensitive(menu_item, (form_pos > 0));
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Move down"), menu, _masks_movedown_callback, gui);
    gtk_widget_set_sensitive(menu_item, (form_pos < list_length - 1));

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  }

  // Risky stuff at the end
  if(gui->creation)
  {
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Done shape creation"), menu,
                                                  _masks_gui_exit_creation_callback, gui);
    menu_item_set_fake_accel(menu_item, GDK_KEY_Escape, 0);
  }
  else
  {
    if(gui->node_hovered >= 0)
    {
      menu_item = ctx_gtk_menu_item_new_with_markup(_("Delete node"), menu, _masks_gui_delete_node_callback, gui);
      menu_item_set_fake_accel(menu_item, GDK_KEY_Delete, 0);
    }
    else
    {
      menu_item = ctx_gtk_menu_item_new_with_markup(_("Remove shape from mask"), menu, _masks_gui_remove_from_group_callback, gui);
      menu_item_set_fake_accel(menu_item, GDK_KEY_Delete, 0);
      gtk_widget_set_sensitive(menu_item, gui->form_selected >= 0);

      menu_item = ctx_gtk_menu_item_new_with_markup(_("Delete shape"), menu, _masks_gui_full_delete_callback, gui);
      gtk_widget_set_sensitive(menu_item, gui->form_selected >= 0);
    }
  }

  for(size_t k = 0; k < G_N_ELEMENTS(op_icon); k++)
    g_clear_object(&op_icon[k]);
  g_clear_object(&composed_icon);

  gtk_widget_show_all(menu);
  return menu;
}

/* De-inlined from masks_gui.h: they dispatch through the private per-shape table. */
gboolean dt_masks_toggle_bezier_node_type(struct dt_iop_module_t *module,
                                                        struct dt_masks_form_t *mask_form,
                                                        struct dt_masks_form_gui_t *mask_gui,
                                                        const int form_index,
                                                        const struct dt_masks_form_gui_points_t *gui_points,
                                                        const int node_index,
                                                        float node[2], float ctrl1[2], float ctrl2[2],
                                                        dt_masks_points_states_t *state)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_gui) || IS_NULL_PTR(gui_points) || IS_NULL_PTR(state) || node_index < 0) return FALSE;

  if(dt_masks_node_is_cusp(gui_points, node_index))
  {
    *state = DT_MASKS_POINT_STATE_NORMAL;
    if(mask_form->functions && mask_form->functions->init_ctrl_points)
      mask_form->functions->init_ctrl_points(mask_form);
  }
  else
  {
    ctrl1[0] = ctrl2[0] = node[0];
    ctrl1[1] = ctrl2[1] = node[1];
    *state = DT_MASKS_POINT_STATE_USER;
  }

  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
  return TRUE;
}

gboolean dt_masks_reset_bezier_ctrl_points(struct dt_iop_module_t *module,
                                                         struct dt_masks_form_t *mask_form,
                                                         struct dt_masks_form_gui_t *mask_gui,
                                                         const int form_index,
                                                         const struct dt_masks_form_gui_points_t *gui_points,
                                                         const int node_index,
                                                         dt_masks_points_states_t *state)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_gui) || IS_NULL_PTR(gui_points) || IS_NULL_PTR(state) || node_index < 0) return FALSE;

  if(*state != DT_MASKS_POINT_STATE_NORMAL && !dt_masks_node_is_cusp(gui_points, node_index))
  {
    *state = DT_MASKS_POINT_STATE_NORMAL;
    if(mask_form->functions && mask_form->functions->init_ctrl_points)
      mask_form->functions->init_ctrl_points(mask_form);
    dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
  }

  return TRUE;
}

/* ------------------------------------------------------------------------------------
 * Everything below was moved verbatim from masks.c: the interactive half -- hit testing,
 * event dispatch, hover/cursor, form_gui lifecycle, creation flows, drawing, menus and
 * the interaction engine. The data model and rasterisation stayed behind.
 * ------------------------------------------------------------------------------------ */

/**
 * @brief Check whether a point lies within a squared radius of a center.
 *
 * Assumptions/caveats:
 * - Uses squared distance to avoid sqrt.
 * - Callers must pass a squared radius (not the radius).
 */
gboolean dt_masks_point_is_within_radius(const float point_x, const float point_y,
                                         const float center_x, const float center_y,
                                         const float squared_radius)
{
  const float delta_x = point_x - center_x;
  const float delta_y = point_y - center_y;
  const float squared_distance = delta_x * delta_x + delta_y * delta_y;
  return squared_distance <= squared_radius;
}

/**
 * @brief Centralized hit-testing for node/handle/segment selection across shapes.
 *
 * This function:
 * - Translates pointer coordinates into GUI space,
 * - Resets selection flags,
 * - Tests border/curve handles and nodes,
 * - Delegates inside/border/segment tests to the shape callback.
 *
 * node_count_override can be used for shapes that don't expose nodes via GList
 * (e.g. gradient/ellipse control points). Pass -1 to use g_list_length().
 *
 * Callers provide shape-specific callbacks for handles and distance tests.
 *
 * The cached cursor in `mask_gui->pos` is authoritative for hit testing.
 */
int dt_masks_find_closest_handle_common(dt_masks_form_t *mask_form,
                                        dt_masks_form_gui_t *mask_gui, int form_index, int node_count_override,
                                        dt_masks_border_handle_fn border_handle_cb,
                                        dt_masks_curve_handle_fn curve_handle_cb,
                                        dt_masks_node_position_fn node_position_cb,
                                        dt_masks_distance_fn distance_cb,
                                        dt_masks_post_select_fn post_select_cb,
                                        void *user_data)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points)) return 0;
  if(!mask_gui->creation && mask_gui->group_selected != form_index) return 0;

  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(IS_NULL_PTR(gui_points)) return 0;

  // Handle detection in backbuffer space.
  const float cursor_radius = DT_GUI_MOUSE_EFFECT_RADIUS;
  const float cursor_radius2 = cursor_radius * cursor_radius;
  const float cursor_x = mask_gui->pos[0];
  const float cursor_y = mask_gui->pos[1];
  const int selected_node = dt_masks_gui_selected_node_index(mask_gui);

  // Keep track of current state, in case we need to refresh total deselection.
  const gboolean need_refresh_anyway = dt_masks_gui_was_anything_selected(mask_gui);

  mask_gui->form_selected = FALSE;
  mask_gui->border_selected = FALSE;
  mask_gui->source_selected = FALSE;

  mask_gui->node_hovered = -1;
  mask_gui->handle_hovered = -1;
  mask_gui->seg_hovered = -1;
  mask_gui->handle_border_hovered = -1;

  if(mask_gui->node_dragging >= 0)
  {
    mask_gui->node_hovered = mask_gui->node_dragging;
    return 1;
  }
  if(mask_gui->handle_dragging >= 0)
  {
    mask_gui->handle_hovered = mask_gui->handle_dragging;
    return 1;
  }
  if(mask_gui->handle_border_dragging >= 0)
  {
    mask_gui->handle_border_hovered = mask_gui->handle_border_dragging;
    return 1;
  }
  if(mask_gui->seg_dragging >= 0)
  {
    mask_gui->seg_hovered = mask_gui->seg_dragging;
    return 1;
  }

  const int node_count = (node_count_override >= 0) ? node_count_override
                                                    : (int)g_list_length(mask_form->points);

  const gboolean has_bezier_layout = mask_form->uses_bezier_points_layout;
  const gboolean can_test_nodes = (!IS_NULL_PTR(node_position_cb)) || has_bezier_layout;
  const int first_node_index = has_bezier_layout ? 0 : 1; // skip center node for non-bezier shapes
  const gboolean has_selected_node = (node_count > 0) && can_test_nodes
                                     && (mask_gui->group_selected == form_index)
                                     && selected_node >= first_node_index && selected_node < node_count;

  if(has_selected_node)
  {
    // Current node's border handle (feather handle).
    float handle_x = NAN;
    float handle_y = NAN;
    if(border_handle_cb
       && border_handle_cb(gui_points, node_count, selected_node, &handle_x, &handle_y, user_data)
       && dt_masks_point_is_within_radius(cursor_x, cursor_y, handle_x, handle_y, cursor_radius2))
    {
      mask_gui->handle_border_hovered = selected_node;
      return 1;
    }

    // Current node's curve handle.
    if(!dt_masks_node_is_cusp(gui_points, selected_node) && curve_handle_cb)
    {
      curve_handle_cb(gui_points, selected_node, &handle_x, &handle_y, user_data);
      if(dt_masks_point_is_within_radius(cursor_x, cursor_y, handle_x, handle_y, cursor_radius2))
      {
        mask_gui->handle_hovered = selected_node;
        return 1;
      }
    }

    // Current node itself.
    float node_x = NAN;
    float node_y = NAN;
    if(node_position_cb)
    {
      node_position_cb(gui_points, selected_node, &node_x, &node_y, user_data);
    }
    else if(has_bezier_layout)
    {
      node_x = gui_points->points[selected_node * 6 + 2];
      node_y = gui_points->points[selected_node * 6 + 3];
    }
    if(!isnan(node_x) && !isnan(node_y)
       && dt_masks_point_is_within_radius(cursor_x, cursor_y, node_x, node_y, cursor_radius2))
    {
      mask_gui->node_hovered = selected_node;
      return 1;
    }
  }

  if(can_test_nodes)
  {
    for(int node_index = first_node_index; node_index < node_count; node_index++)
    {
      float node_x = NAN;
      float node_y = NAN;
      if(node_position_cb)
      {
        node_position_cb(gui_points, node_index, &node_x, &node_y, user_data);
      }
      else if(has_bezier_layout)
      {
        node_x = gui_points->points[node_index * 6 + 2];
        node_y = gui_points->points[node_index * 6 + 3];
      }
      if(!isnan(node_x) && !isnan(node_y)
         && dt_masks_point_is_within_radius(cursor_x, cursor_y, node_x, node_y, cursor_radius2))
      {
        mask_gui->node_hovered = node_index;
        return 1;
      }
    }
  }

  if(IS_NULL_PTR(distance_cb)) return 0;

  // Segment or shape hit tests.
  int inside = 0;
  int inside_border = 0;
  int near_segment = -1;
  int inside_source = 0;
  float nearest_distance = 0.0f;
  distance_cb(cursor_x, cursor_y, cursor_radius, mask_gui, form_index, node_count, &inside, &inside_border,
              &near_segment, &inside_source, &nearest_distance, user_data);


  if(inside_source)
  {
    mask_gui->form_selected = TRUE;
    mask_gui->source_selected = TRUE;
    if(post_select_cb) post_select_cb(mask_gui, inside, inside_border, inside_source, user_data);
    return 1;
  }
  if(inside_border)
  {
    mask_gui->form_selected = TRUE;
    mask_gui->border_selected = TRUE;
    if(post_select_cb) post_select_cb(mask_gui, inside, inside_border, inside_source, user_data);
    return 1;
  }
  if(near_segment >= 0)
  {
    if(near_segment < node_count)
      mask_gui->seg_hovered = near_segment;
    return 1;
  }
  if(inside)
  {
    mask_gui->form_selected = TRUE;
    if(post_select_cb) post_select_cb(mask_gui, inside, inside_border, inside_source, user_data);
    return 1;
  }

  // Deselection needs a refresh at least once.
  return need_refresh_anyway;
}

/**
 * @brief Find the group-membership entry for `form_id` inside `group_form`'s own `points`
 * list (not recursive into subgroups) -- the shared primitive behind every "find this shape's
 * row within its parent group" call site.
 *
 * Assumption: only valid for DT_MASKS_GROUP forms.
 * @param out_index if non-NULL, receives the entry's position in the list (-1 if not found).
 */
dt_masks_form_group_t *dt_masks_form_group_find_entry(dt_masks_form_t *group_form, const int form_id, int *out_index)
{
  if(out_index) *out_index = -1;
  if(IS_NULL_PTR(group_form) || !(group_form->type & DT_MASKS_GROUP)) return NULL;

  // Iterate group entries to find the matching form id.
  int index = 0;
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    if(group_entry && group_entry->formid == form_id)
    {
      if(out_index) *out_index = index;
      return group_entry;
    }
    index++;
  }
  return NULL;
}

int dt_masks_group_index_from_formid(const dt_masks_form_t *group_form, int form_id)
{
  if(IS_NULL_PTR(group_form) || !(group_form->type & DT_MASKS_GROUP)) return -1;

  int index = 0;
  for(const GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    const dt_masks_form_group_t *group_entry = (const dt_masks_form_group_t *)group_node->data;
    if(!IS_NULL_PTR(group_entry) && group_entry->formid == form_id) return index;
    index++;
  }
  return -1;
}

/**
 * @brief Return the currently visible form used by the masks GUI.
 *
 * This can be a temporary group copy used for editing, not necessarily a form
 * stored in dev->forms.
 */
dt_masks_form_t *dt_masks_get_visible_form(const dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->form_gui)) return NULL;
  return dev->form_gui->form_visible;
}

void dt_masks_set_visible_form(dt_develop_t *dev, dt_masks_form_t *form)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->form_gui)) return;
  dev->form_gui->form_visible = form;
}

void dt_masks_gui_init(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return;

  if(IS_NULL_PTR(dev->form_gui))
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev, dev->form_gui);
  }

  dt_masks_clear_form_gui(dev);
  dt_masks_set_visible_form(dev, NULL);
  dev->form_gui->geometry_generation = 0;
  dev->form_gui->formid = 0;
}

void dt_masks_gui_cleanup(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->form_gui)) return;

  // If shutdown happens while a shape is being created, release the unfinished
  // temporary visible form before clearing the GUI state.
  dt_masks_form_exit_creation(NULL, dev->form_gui);
  
  dt_masks_clear_form_gui(dev);
  dt_free(dev->form_gui);
  dt_masks_set_visible_form(dev, NULL);
}

void dt_masks_gui_set_dragging(dt_masks_form_gui_t *gui)
{
  if(IS_NULL_PTR(gui)) return;

  if(gui->handle_selected && gui->handle_hovered >= 0) gui->handle_dragging = gui->handle_hovered;
  if(gui->handle_border_selected && gui->handle_border_hovered >= 0) gui->handle_border_dragging = gui->handle_border_hovered;
  if(gui->node_selected && gui->node_hovered >= 0) gui->node_dragging = gui->node_hovered;
  if(gui->seg_selected && gui->seg_hovered >= 0) gui->seg_dragging = gui->seg_hovered;
  if(gui->source_selected)
    gui->source_dragging = TRUE;
  else if(gui->form_selected)
    gui->form_dragging = TRUE;
}

void dt_masks_gui_reset_dragging(dt_masks_form_gui_t *gui)
{
  if(IS_NULL_PTR(gui)) return;

  gui->handle_dragging = -1;
  gui->handle_border_dragging = -1;
  gui->node_dragging = -1;
  gui->seg_dragging = -1;
  gui->form_dragging = FALSE;
  gui->source_dragging = FALSE;
}

gboolean dt_masks_gui_is_dragging(const dt_masks_form_gui_t *gui)
{
  if(IS_NULL_PTR(gui)) return FALSE;
  const gboolean dragging = (gui->form_dragging || gui->source_dragging || gui->seg_dragging >= 0 || gui->node_dragging >= 0
                              || gui->handle_dragging >= 0 || gui->handle_border_dragging >= 0);
  dt_control_mouse_is_dragging(dragging);
  return dragging;
}

/**
 * @brief Return the group entry for a (parent, form) pair.
 *
 * Caveat: returns NULL if parent isn't a group or the entry is missing.
 */
dt_masks_form_group_t *dt_masks_form_group_from_parentid(dt_develop_t *dev, int parent_id, int form_id)
{
  dt_masks_form_t *group_form = dt_masks_get_from_id(dev, parent_id);
  if(IS_NULL_PTR(group_form) || !(group_form->type & DT_MASKS_GROUP)) return NULL;
  return dt_masks_form_group_find_entry(group_form, form_id, NULL);
}

// Read-only recursive search: dev->forms's top-level groups and their nested subgroups.
// grp == NULL starts the search at dev->forms itself. max_depth bounds the recursion so a
// corrupted or maliciously crafted masks_history (a group referencing an ancestor of itself --
// dt_masks_group_add_form guards against this interactively via _find_in_group, but a raw
// DB/XMP load does not validate it) cannot stack-overflow the caller; the UI never nests
// groups anywhere near this deep (see the `depth < 3` guards in libs/masks.c).

/**
 * @brief Get the selected group entry from the GUI selection index.
 *
 * Selection sequence overview:
 * - The GUI stores a "working" selection index in dt_masks_form_gui_t::group_selected.
 *   This is fast for UI interaction but can become stale when the group list mutates
 *   (insert/remove/reorder or reallocated nodes).
 * - dt_masks_form_get_selected_group() uses that index directly. It assumes the list
 *   is unchanged since the GUI selection was set.
 * - dt_masks_form_get_selected_group_live() resolves the selection more safely by:
 *   1) attempting the GUI index,
 *   2) re-resolving through parentid/formid to refresh the pointer if needed.
 *
 * Use dt_masks_form_get_selected_group() in tight GUI paths where the list is known
 * stable; use dt_masks_form_get_selected_group_live() when correctness matters across
 * potential list mutations.
 *
 * @todo simplify that.
 */
dt_masks_form_group_t *dt_masks_form_get_selected_group(const dt_masks_form_t *mask_form,
                                                        const dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_gui)) return NULL;
  if(mask_gui->group_selected < 0) return NULL;
  return (dt_masks_form_group_t *)g_list_nth_data(mask_form->points, mask_gui->group_selected);
}

/**
 * @brief Resolve a "live" selected group entry, even if GUI selection is stale.
 *
 * Selection source:
 * - GUI index (mask_gui->group_selected) for the currently visible group.
 *
 * If the GUI works on a temporary group copy, we re-resolve through parentid
 * to get the live entry from dev->forms.
 */
dt_masks_form_group_t *dt_masks_form_get_selected_group_live(const dt_masks_form_t *mask_form,
                                                             const dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_gui)) return NULL;

  dt_masks_form_group_t *selected_group_entry = NULL;
  if(mask_gui->group_selected >= 0)
    selected_group_entry = dt_masks_form_get_selected_group(mask_form, mask_gui);

  if(IS_NULL_PTR(selected_group_entry)) return NULL;

  if(selected_group_entry->parentid > 0)
  {
    // Re-resolve via parentid to ensure the pointer is still current.
    dt_masks_form_group_t *resolved_group_entry
        = dt_masks_form_group_from_parentid(mask_gui->dev, selected_group_entry->parentid,
                                            selected_group_entry->formid);
    if(resolved_group_entry) return resolved_group_entry;
  }

  return selected_group_entry;
}

/**
 * @brief Resolve the concrete form that should receive an event.
 *
 * Visible groups expose one selected child to the event path. Non-group forms
 * dispatch to themselves.
 */
static dt_masks_form_t *_dt_masks_events_get_dispatch_form(dt_masks_form_t *visible_form,
                                                           const dt_masks_form_gui_t *mask_gui,
                                                           dt_masks_form_group_t **group_entry,
                                                           int *parent_id, int *form_index)
{
  if(group_entry) *group_entry = NULL;
  if(parent_id) *parent_id = 0;
  if(form_index) *form_index = 0;

  if(IS_NULL_PTR(visible_form)) return NULL;
  if(!(visible_form->type & DT_MASKS_GROUP)) return visible_form;

  dt_masks_form_group_t *selected_group_entry
      = dt_masks_form_get_selected_group_live(visible_form, mask_gui);
  if(IS_NULL_PTR(selected_group_entry)) return NULL;

  dt_masks_form_t *selected_form = dt_masks_get_from_id(mask_gui->dev, selected_group_entry->formid);
  if(IS_NULL_PTR(selected_form)) return NULL;

  if(group_entry) *group_entry = selected_group_entry;
  if(parent_id) *parent_id = selected_group_entry->parentid;
  if(form_index) *form_index = mask_gui->group_selected;

  return selected_form;
}

/**
 * @brief Update group selection from the current cached cursor before leaf dispatch.
 *
 * If a handle on the currently selected child is already hovered, keep that child selected.
 * Otherwise, fall back to per-shape hit testing to resolve the leaf form under the cursor.
 */
static gboolean _dt_masks_events_group_update_selection(dt_masks_form_t *group_form,
                                                        dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(group_form) || IS_NULL_PTR(mask_gui)) return FALSE;

  dt_develop_t *const dev = mask_gui->dev;
  const float radius = DT_GUI_MOUSE_EFFECT_RADIUS;
  const float cursor_x = mask_gui->pos[0];
  const float cursor_y = mask_gui->pos[1];
  const int prev_group_selected = mask_gui->group_selected;
  const gboolean prev_border_selected = mask_gui->border_selected;
  int locked_formid = -1;

  if(dt_masks_is_anything_hovered(mask_gui))
    return TRUE;

  if(prev_border_selected && prev_group_selected >= 0)
  {
    dt_masks_form_group_t *selected_group_entry = dt_masks_form_get_selected_group_live(group_form, mask_gui);
    dt_masks_form_t *selected_form = selected_group_entry
                                         ? dt_masks_get_from_id(dev, selected_group_entry->formid)
                                         : NULL;
    const gboolean has_border_lock_candidate = selected_group_entry
                                               && selected_form
                                               && (selected_form->type & DT_MASKS_IS_CLOSED_SHAPE)
                                               && selected_form->functions
                                               && selected_form->functions->get_distance;
    if(has_border_lock_candidate)
    {
      // Lock selection only when the click lands on the selected closed-shape border/segment.
      int inside = 0;
      int inside_border = 0;
      int near_handle = -1;
      int inside_source = 0;
      float dist = FLT_MAX;
      selected_form->functions->get_distance(cursor_x, cursor_y, radius, mask_gui, prev_group_selected,
                                             g_list_length(selected_form->points), &inside, &inside_border,
                                             &near_handle, &inside_source, &dist);
      if(inside_border || near_handle >= 0)
        locked_formid = selected_group_entry->formid;
    }
  }

  if(prev_group_selected >= 0)
    dt_masks_soft_reset_form_gui(mask_gui);

  dt_masks_form_t *selected_form = NULL;
  int selected_index = -1;
  float best_dist = FLT_MAX;

  int index = 0;
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node), index++)
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    if(IS_NULL_PTR(group_entry)) continue;
    if(locked_formid >= 0 && group_entry->formid != locked_formid) continue;

    dt_masks_form_t *form = dt_masks_get_from_id(dev, group_entry->formid);
    if(IS_NULL_PTR(form)) continue;

    int inside = 0;
    int inside_border = 0;
    int near_handle = -1;
    int inside_source = 0;
    float dist = FLT_MAX;
    if(form->functions && form->functions->get_distance)
      form->functions->get_distance(cursor_x, cursor_y, radius, mask_gui, index, g_list_length(form->points),
                                    &inside, &inside_border, &near_handle, &inside_source, &dist);

    const gboolean is_selected_form = (prev_group_selected == index);
    const gboolean hit_border = (inside_border || near_handle >= 0);
    const gboolean is_open_shape = (form->type & DT_MASKS_IS_OPEN_SHAPE) != 0;
    // Only open shapes can be selected via their border when unselected.
    if(!is_selected_form && hit_border && !is_open_shape)
      continue;

    if(inside || hit_border || inside_source)
    {
      // Lazily computed: only shapes actually hit by this click need it, not every member
      // of the group on every mouse event.
      if(!form->gravity_center_valid) dt_masks_form_update_gravity_center(mask_gui->dev, form);
      const float dx = mask_gui->raw_pos[0] - form->gravity_center[0];
      const float dy = mask_gui->raw_pos[1] - form->gravity_center[1];
      const float center_dist2 = dx * dx + dy * dy;
      const float combined_dist2 = dist * center_dist2;
      if(combined_dist2 < best_dist)
      {
        selected_form = form;
        selected_index = index;
        best_dist = combined_dist2;
      }
    }
  }

  if(!IS_NULL_PTR(selected_form))
  {
    mask_gui->group_selected = selected_index;
    return TRUE;
  }

  return mask_gui->group_selected >= 0;
}

static gboolean _dt_masks_events_should_update_hover_on_move(dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(mask_gui) || mask_gui->creation) return FALSE;
  if(mask_gui->form_rotating || mask_gui->border_toggling || mask_gui->gradient_toggling) return FALSE;
  if(dt_masks_gui_is_dragging(mask_gui)) return FALSE;
  return dt_masks_gui_should_hit_test(mask_gui);
}

static int _dt_masks_events_update_hover(dt_masks_form_t *dispatch_form, dt_masks_form_gui_t *mask_gui,
                                          const int form_index)
{
  if(IS_NULL_PTR(dispatch_form) || IS_NULL_PTR(mask_gui) || !dispatch_form->functions || !dispatch_form->functions->update_hover)
    return 0;
  return dispatch_form->functions->update_hover(dispatch_form, mask_gui, form_index);
}

static gboolean _dt_masks_events_cursor_over_form(const dt_masks_form_t *dispatch_form,
                                                  dt_masks_form_gui_t *mask_gui,
                                                  const int form_index)
{
  if(!dispatch_form || IS_NULL_PTR(mask_gui) || !dispatch_form->functions || !dispatch_form->functions->get_distance)
    return FALSE;

  int inside = 0;
  int inside_border = 0;
  int near_handle = -1;
  int inside_source = 0;
  float dist = FLT_MAX;
  dispatch_form->functions->get_distance(mask_gui->pos[0], mask_gui->pos[1], DT_GUI_MOUSE_EFFECT_RADIUS,
                                         mask_gui, form_index,
                                         g_list_length(dispatch_form->points), &inside, &inside_border, &near_handle,
                                         &inside_source, &dist);
  return inside || inside_border || near_handle >= 0 || inside_source;
}

/**
 * @brief Consume the initial drag motion used to disambiguate scrolling vs dragging in groups.
 */
static gboolean _dt_masks_events_group_blocks_motion(dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(mask_gui)) return FALSE;

  const float radius = DT_GUI_MOUSE_EFFECT_RADIUS;
  if(mask_gui->scrollx == 0.0f || mask_gui->scrolly == 0.0f) return FALSE;

  if((mask_gui->scrollx - mask_gui->pos[0] < radius && mask_gui->scrollx - mask_gui->pos[0] > -radius)
     && (mask_gui->scrolly - mask_gui->pos[1] < radius && mask_gui->scrolly - mask_gui->pos[1] > -radius))
    return TRUE;

  mask_gui->scrollx = 0.0f;
  mask_gui->scrolly = 0.0f;
  return FALSE;
}

/**
 * @brief Flush a deferred throttled rebuild before drag state is reset.
 */
static gboolean _dt_masks_events_flush_rebuild_if_needed(struct dt_iop_module_t *module,
                                                         dt_masks_form_t *dispatch_form,
                                                         dt_masks_form_gui_t *mask_gui,
                                                         const int form_index, const int button)
{
  if(button != 1) return FALSE;
  if(!dt_masks_gui_is_dragging(mask_gui)) return FALSE;

  if(mask_gui->rebuild_pending)
  {
    if(!IS_NULL_PTR(dispatch_form))
    {
      dt_masks_gui_form_create(dispatch_form, mask_gui, form_index, module);

      dt_develop_t *const dev = mask_gui->dev;
      if(!IS_NULL_PTR(dev))
      {
        mask_gui->last_rebuild_ts = dt_get_wtime();
        mask_gui->last_rebuild_pos[0] = mask_gui->pos[0];
        mask_gui->last_rebuild_pos[1] = mask_gui->pos[1];
      }
    }
    mask_gui->rebuild_pending = FALSE;
  }

  return TRUE;
}

/**
 * @brief Build and display the on-canvas hint message for masks interactions.
 *
 * Pitfall: set_hint_message() may rely on gui->form_selected, so we may need
 *          a temporary override when no hint is produced.
 */
static gboolean _set_hinter_message(dt_masks_form_gui_t *mask_gui, const dt_masks_form_t *mask_form)
{
  // Sized for the longest hint once translated: a truncated message would also cut a Pango
  // markup tag in half, and dt_hinter_set_message() feeds the result to gtk_label_set_markup().
  char message[512] = "";

  // Checked before use, not after: this function tests IS_NULL_PTR(mask_form) further down
  // and its own dt_print writes `mask_form ? mask_form->type : -1`, so a NULL was always
  // considered possible here -- but form_type read it unconditionally first.
  if(IS_NULL_PTR(mask_form)) return FALSE;

  const int form_type = mask_form->type;

  const dt_masks_form_t *selected_form = mask_form;
  int selected_form_id = 0;
  if(!IS_NULL_PTR(mask_form) && (mask_form->type & DT_MASKS_GROUP))
  {
    const dt_masks_form_group_t *selected_group_entry
        = dt_masks_form_get_selected_group_live(mask_form, mask_gui);
    if(!IS_NULL_PTR(selected_group_entry)) selected_form_id = selected_group_entry->formid;
  }

  dt_print(DT_DEBUG_INPUT,
           "[masks] hint begin: form=%p type=%d gui=%p group_selected=%d form_selected=%d node_hovered=%d seg_hovered=%d selected_formid=%d\n",
           (void *)mask_form, mask_form ? mask_form->type : -1, (void *)mask_gui,
           mask_gui->group_selected, mask_gui->form_selected,
           mask_gui->node_hovered, mask_gui->seg_hovered,
           selected_form_id);
  if(form_type & DT_MASKS_GROUP)
  {
    // Resolve the selected form inside a group (if any).
    dt_masks_form_group_t *selected_group_entry = dt_masks_form_get_selected_group_live(mask_form, mask_gui);
    if(!IS_NULL_PTR(selected_group_entry))
    {
      selected_form = dt_masks_get_from_id(mask_gui->dev, selected_group_entry->formid);
      if(IS_NULL_PTR(selected_form)) return FALSE;
    }
  }

  if(selected_form->functions && selected_form->functions->set_hint_message)
  {
    selected_form->functions->set_hint_message(mask_gui, selected_form, message, sizeof(message));
  }

  dt_control_hinter_message(dt_control_get_global(), message);
  dt_print(DT_DEBUG_INPUT,
           "[masks] hint end: sel=%p has_set_hint=%d msg_len=%" G_GSIZE_FORMAT " msg='%s'\n",
           (void *)selected_form,
           (selected_form && selected_form->functions && selected_form->functions->set_hint_message) ? 1 : 0,
           strlen(message), message);
  return message[0] != '\0';
}

void dt_masks_init_form_gui(dt_develop_t *dev, dt_masks_form_gui_t *mask_gui)
{
  memset(mask_gui, 0, sizeof(dt_masks_form_gui_t));

  mask_gui->dev = dev;
  mask_gui->pos[0] = mask_gui->pos[1] = -1.0f;
  mask_gui->rel_pos[0] = mask_gui->rel_pos[1] = -1.0f;
  mask_gui->raw_pos[0] = mask_gui->raw_pos[1] = -1.0f;
  mask_gui->pos_source[0] = mask_gui->pos_source[1] = -1.0f;
  mask_gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
  mask_gui->node_hovered = -1;
  mask_gui->handle_hovered = -1;
  mask_gui->seg_hovered = -1;
  mask_gui->handle_border_hovered = -1;
  mask_gui->node_selected = FALSE;
  mask_gui->handle_selected = FALSE;
  mask_gui->seg_selected = FALSE;
  mask_gui->handle_border_selected = FALSE;
  mask_gui->node_selected_idx = -1;
  mask_gui->form_selected = FALSE;
  mask_gui->border_selected = FALSE;
  mask_gui->source_selected = FALSE;
  mask_gui->pivot_selected = FALSE;
  mask_gui->last_rebuild_ts = 0.0;
  mask_gui->last_rebuild_pos[0] = mask_gui->last_rebuild_pos[1] = 0.0f;
  mask_gui->rebuild_pending = FALSE;
  mask_gui->last_hit_test_pos[0] = mask_gui->last_hit_test_pos[1] = -1.0f;
}

void dt_masks_soft_reset_form_gui(dt_masks_form_gui_t *mask_gui)
{
  // Note: we have an hard reset function below that frees all buffers and such
  mask_gui->source_selected = FALSE;
  mask_gui->node_hovered = -1;
  mask_gui->handle_hovered = -1;
  mask_gui->seg_hovered = -1;
  mask_gui->handle_border_hovered = -1;
  mask_gui->node_selected = FALSE;
  mask_gui->handle_selected = FALSE;
  mask_gui->seg_selected = FALSE;
  mask_gui->handle_border_selected = FALSE;
  mask_gui->node_selected_idx = -1;
  mask_gui->group_selected = -1;
  mask_gui->delta[0] = mask_gui->delta[1] = 0.0f;
  mask_gui->form_selected = mask_gui->border_selected = mask_gui->form_dragging = mask_gui->form_rotating = FALSE;
  mask_gui->pivot_selected = FALSE;
  mask_gui->handle_border_dragging = mask_gui->seg_dragging = mask_gui->handle_dragging = mask_gui->node_dragging = -1;
  mask_gui->last_rebuild_ts = 0.0;
  mask_gui->last_rebuild_pos[0] = mask_gui->last_rebuild_pos[1] = 0.0f;
  mask_gui->rebuild_pending = FALSE;
  mask_gui->last_hit_test_pos[0] = mask_gui->last_hit_test_pos[1] = -1.0f;
}

void dt_masks_gui_form_create(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui,
                              int form_index, dt_iop_module_t *module)
{
  // Never guarded before the move; every branch below reads mask_gui->dev, and the
  // analyzer is right that nothing upstream promises it is set.
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_gui) || IS_NULL_PTR(mask_gui->dev)) return;

  const int gui_points_count = g_list_length(mask_gui->points);
  if(gui_points_count == form_index)
  {
    dt_masks_form_gui_points_t *gui_points_new
        = (dt_masks_form_gui_points_t *)calloc(1, sizeof(dt_masks_form_gui_points_t));
    mask_gui->points = g_list_append(mask_gui->points, gui_points_new);
  }
  else if(gui_points_count < form_index)
    return;

  dt_masks_gui_form_remove(mask_form, mask_gui, form_index);

  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  const dt_masks_raster_result_t border_status
      = dt_masks_get_points_border(mask_gui->dev, mask_form, &gui_points->points, &gui_points->points_count,
                                   &gui_points->border, &gui_points->border_count,
                                   &gui_points->border_skips, &gui_points->border_skip_count, 0, NULL);

  /* Only a genuine FAILURE may leave the cache key unset, and it must: the key covers the whole
   * group, so one unstamped shape rebuilds every shape of the group on every later expose --
   * forever, until the geometry moves.
   *
   * That is precisely why "this shape has no outline" and "building the outline broke" had to
   * stop sharing one return value. A shape with no geometry HAS an outline -- the empty one --
   * and caching it is correct; the drawing code already skips a NULL outline. A build that
   * failed produced nothing to cache and must be retried. Three of the five shapes used to
   * report the first case as success, so the cache was stamped over a NULL outline and the
   * shape stayed invisible until the geometry next moved. */
  if(border_status == DT_MASKS_RASTER_ERROR && (dt_get_debug_flags() & DT_DEBUG_MASKS))
    dt_print(DT_DEBUG_MASKS,
             "[masks] outline build FAILED for %s (index %d, %d nodes): the cache key stays unset,"
             " so every later expose rebuilds this whole group\n",
             mask_form->name, form_index, g_list_length(mask_form->points));

  if(border_status != DT_MASKS_RASTER_ERROR)
  {
    if(border_status == DT_MASKS_RASTER_OK && (mask_form->type & DT_MASKS_CLONE))
    {
      if(dt_masks_get_points_border(mask_gui->dev, mask_form, &gui_points->source, &gui_points->source_count,
                                    NULL, NULL, NULL, NULL, TRUE, module)
         != DT_MASKS_RASTER_OK)
        return;
    }
    mask_gui->geometry_generation = dt_geometry_chain_generation(mask_gui->dev->geometry_chain);
    mask_gui->formid = mask_form->formid;
    mask_gui->type = mask_form->type;

  }

  dt_masks_form_update_gravity_center(mask_gui->dev, mask_form);
}

gboolean dt_masks_gui_form_create_throttled(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui,
                                            int form_index, dt_iop_module_t *module,
                                            float posx, float posy)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_gui)) return FALSE;

  dt_develop_t *const develop = mask_gui->dev;
  if(IS_NULL_PTR(develop))
  {
    dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
    return TRUE;
  }

  const double now = dt_get_wtime();
  const double min_delta_time = 1.0 / 60.0;
  const float min_dist2 = 4.0f;
  /* Bypass the throttle only when the GEOMETRY moved -- the outlines would then be drawn in the
   * wrong place, which no amount of throttling makes acceptable. It used to bypass on the preview
   * pipe's backbuffer hash instead, so a republished frame counted as a reason: dragging a brush
   * republishes continuously, the clause was therefore true on essentially every mouse move, and
   * the throttle below never ran. #1158. */
  const gboolean force_rebuild
      = (mask_gui->geometry_generation != dt_geometry_chain_generation(develop->geometry_chain));

  if(!force_rebuild && mask_gui->last_rebuild_ts > 0.0)
  {
    const double elapsed_time = now - mask_gui->last_rebuild_ts;
    const float delta_x = posx - mask_gui->last_rebuild_pos[0];
    const float delta_y = posy - mask_gui->last_rebuild_pos[1];
    if(elapsed_time < min_delta_time && (delta_x * delta_x + delta_y * delta_y) < min_dist2)
    {
      mask_gui->rebuild_pending = TRUE;
      return FALSE;
    }
  }

  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
  mask_gui->last_rebuild_ts = now;
  mask_gui->last_rebuild_pos[0] = posx;
  mask_gui->last_rebuild_pos[1] = posy;
  mask_gui->rebuild_pending = FALSE;
  return TRUE;
}

void dt_masks_remove_node(struct dt_iop_module_t *module, dt_masks_form_t *mask_form, int parent_id,
                          dt_masks_form_gui_t *mask_gui, int form_index, int node_index)
{
  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->points)) return;
  mask_form = dt_masks_cow_touch(mask_gui->dev, mask_form);
  dt_masks_node_brush_t *brush_node = (dt_masks_node_brush_t *)g_list_nth_data(mask_form->points, node_index);
  if(IS_NULL_PTR(brush_node)) return;
  mask_form->points = g_list_remove(mask_form->points, brush_node);
  dt_free(brush_node);
  mask_gui->node_hovered = -1;
  mask_gui->node_selected = FALSE;
  mask_gui->node_selected_idx = -1;
  if(mask_form->functions && mask_form->functions->init_ctrl_points)
    mask_form->functions->init_ctrl_points(mask_form);
    
  // we recreate the form points
  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
}

/**
 * @brief Remove a shape from the GUI and free its resources.
 * 
 * @param module The module owning the mask
 * @param form The form to remove
 * @param parentid The parent ID of the form
 * @param gui The GUI state
 * @param index The index of the form in the group
 * 
 * @return gboolean TRUE if the form was removed, FALSE otherwise.
 */
static gboolean _masks_remove_shape(struct dt_iop_module_t *module, dt_masks_form_t *mask_form, int parent_id,
                                    dt_masks_form_gui_t *mask_gui, int form_index)
{
  // if the form doesn't below to a group, we don't delete it
  if(parent_id <= 0) return 1;

  // we hide the form
  dt_masks_form_t *visible_form = dt_masks_get_visible_form(mask_gui->dev);
  if(IS_NULL_PTR(visible_form) || !(visible_form->type & DT_MASKS_GROUP))
    dt_masks_change_form_gui(mask_gui->dev, NULL);
  else if(g_list_shorter_than(visible_form->points, 2))
    dt_masks_change_form_gui(mask_gui->dev, NULL);
  else
  {
    const int edit_mode = mask_gui->edit_mode;

    dt_masks_clear_form_gui(mask_gui->dev);
    visible_form = dt_masks_cow_touch(mask_gui->dev, visible_form);
    // Remove the selected shape copy from the visible group before deleting the
    // real group entry below.
    for(GList *forms = visible_form->points; forms; forms = g_list_next(forms))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)forms->data;
      if(group_entry->formid == mask_form->formid)
      {
        visible_form->points = g_list_remove(visible_form->points, group_entry);
        dt_free(group_entry);
        break;
      }
    }
    mask_gui->edit_mode = edit_mode;
  }

  // we delete or remove the shape
  // Called from node removal, if there was not enough nodes to keep the whole shape,
  // that's how this was called:
  // dt_masks_form_remove(module, NULL, form);
  // Called from shape removal, this is how it was called:
  dt_masks_form_delete(mask_gui->dev, module, dt_masks_get_from_id(mask_gui->dev, parent_id), mask_form);
  // Not sure what difference it makes.

  return 1;
}

static int _masks_gui_form_group_use_count(const dt_develop_t *dev, const int formid)
{
  if(IS_NULL_PTR(dev)) return 0;

  int count = 0;
  for(GList *form_node = dev->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *group_form = (dt_masks_form_t *)form_node->data;
    if(IS_NULL_PTR(group_form) || !(group_form->type & DT_MASKS_GROUP)) continue;

    for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
      if(group_entry && group_entry->formid == formid)
      {
        count++;
        if(count > 1) goto done;
        break;
      }
    }
  }

done:
  return count;
}

// Shared tail of dt_masks_remove_or_delete()/dt_masks_remove_shape_from_group()/dt_masks_delete_shape():
// once it's decided whether the form is kept unused or fully deleted, the group-selection
// bookkeeping, history commit and signal raising are identical.
static gboolean _masks_remove_or_delete_finish(struct dt_iop_module_t *module, dt_masks_form_t *sel, int parent_id,
                                               dt_masks_form_gui_t *mask_gui, int form_id, gboolean keep_unused)
{
  dt_masks_form_t *visible_form = dt_masks_get_visible_form(mask_gui->dev);
  int next_form_index = -1;
  int next_formid = 0;
  if(!IS_NULL_PTR(visible_form) && (visible_form->type & DT_MASKS_GROUP)
     && mask_gui->group_selected >= 0 && !g_list_shorter_than(visible_form->points, 2))
  {
    const int group_length = g_list_length(visible_form->points);
    next_form_index = mask_gui->group_selected < group_length - 1
                          ? mask_gui->group_selected
                          : mask_gui->group_selected - 1;

    // Walk from the selected row to remember the shape that should be selected
    // after deletion: next row first, previous row if the removed row is last.
    GList *selected_node = g_list_nth(visible_form->points, mask_gui->group_selected);
    if(!IS_NULL_PTR(selected_node))
    {
      GList *next_node = g_list_next(selected_node);
      if(IS_NULL_PTR(next_node)) next_node = g_list_previous(selected_node);
      if(!IS_NULL_PTR(next_node))
      {
        dt_masks_form_group_t *next_group_entry = (dt_masks_form_group_t *)next_node->data;
        if(!IS_NULL_PTR(next_group_entry)) next_formid = next_group_entry->formid;
      }
    }
  }

  gboolean res = TRUE;
  int signal_parent_id = 0;
  dt_masks_event_t signal_event = DT_MASKS_EVENT_REMOVE;

  if(keep_unused)
  {
    // Only remove from current group, keep the form itself for potential reuse.
    res = _masks_remove_shape(module, sel, parent_id, mask_gui, mask_gui->group_selected);
    signal_parent_id = parent_id;
    signal_event = DT_MASKS_EVENT_DELETE;
  }

  else // Permanent delete.
  {
    if(IS_NULL_PTR(visible_form) || !(visible_form->type & DT_MASKS_GROUP))
      dt_masks_change_form_gui(mask_gui->dev, NULL);
    else if(g_list_shorter_than(visible_form->points, 2))
      dt_masks_change_form_gui(mask_gui->dev, NULL);
    else
    {
      const int edit_mode = mask_gui->edit_mode;
      dt_masks_clear_form_gui(mask_gui->dev);
      visible_form = dt_masks_cow_touch(mask_gui->dev, visible_form);
      // Remove the selected shape copy from the visible group before deleting
      // the real form from the develop mask list below.
      for(GList *forms = visible_form->points; forms; forms = g_list_next(forms))
      {
        dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)forms->data;
        if(group_entry->formid == sel->formid)
        {
          visible_form->points = g_list_remove(visible_form->points, group_entry);
          dt_free(group_entry);
          break;
        }
      }
      mask_gui->edit_mode = edit_mode;
      if(next_formid > 0)
      {
        mask_gui->group_selected = next_form_index;
        mask_gui->form_selected = TRUE;
      }
    }

    dt_masks_form_delete(mask_gui->dev, module, NULL, sel);
  }

  dt_dev_add_history_item(mask_gui->dev, module, TRUE, TRUE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, form_id, signal_parent_id,
                                signal_event);

  if(res && next_formid > 0)
  {
    // The mask manager rebuilds its tree on the delete/remove signal, so apply
    // the replacement selection after the signal has finished refreshing lists.
    mask_gui->group_selected = next_form_index;
    mask_gui->form_selected = TRUE;
    dt_dev_masks_selection_change(mask_gui->dev, module, next_formid, FALSE);
    dt_masks_select_form(mask_gui->dev, module, dt_masks_get_from_id(mask_gui->dev, next_formid));
  }
  return res;
}

gboolean dt_masks_remove_or_delete(struct dt_iop_module_t *module, dt_masks_form_t *sel, int parent_id,
                                    dt_masks_form_gui_t *mask_gui, int form_id)
{
  const int use_count = _masks_gui_form_group_use_count(mask_gui->dev, form_id);

  // We don't ask for confirmation if the module uses internal masks,
  // just delete the form as it won't be visible in the shape manager.
  const gboolean internal_masks
      = !IS_NULL_PTR(module)
        && ((module->flags() & IOP_FLAGS_INTERNAL_MASKS) == IOP_FLAGS_INTERNAL_MASKS);

  int response = GTK_RESPONSE_YES;
  if(use_count <= 1 && !internal_masks)
  {
    response = dt_masks_gui_confirm_delete_form_dialog(sel->name);
    if(response == GTK_RESPONSE_CANCEL) return FALSE;
  }

  return _masks_remove_or_delete_finish(module, sel, parent_id, mask_gui, form_id, response == GTK_RESPONSE_NO);
}

gboolean dt_masks_remove_shape_from_group(struct dt_iop_module_t *module, dt_masks_form_t *sel, int parent_id,
                                          dt_masks_form_gui_t *mask_gui, int form_id)
{
  if(IS_NULL_PTR(sel) || IS_NULL_PTR(mask_gui)) return FALSE;
  return _masks_remove_or_delete_finish(module, sel, parent_id, mask_gui, form_id, TRUE);
}

gboolean dt_masks_delete_shape(struct dt_iop_module_t *module, dt_masks_form_t *sel, int parent_id,
                               dt_masks_form_gui_t *mask_gui, int form_id)
{
  if(IS_NULL_PTR(sel) || IS_NULL_PTR(mask_gui)) return FALSE;
  if(!dt_masks_gui_confirm_permanent_delete(sel->name)) return FALSE;
  return _masks_remove_or_delete_finish(module, sel, parent_id, mask_gui, form_id, FALSE);
}

gboolean dt_masks_form_exit_creation(dt_iop_module_t *module, dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(mask_gui)) return FALSE;

  if(mask_gui->creation)
  {
    const int last_formid = mask_gui->creation_last_formid;
    dt_iop_module_t *creation_module = mask_gui->creation_module ? mask_gui->creation_module : module;
    dt_develop_t *dev = mask_gui->dev;
    dt_masks_form_t *temporary_form = dt_masks_get_visible_form(dev);

    if(mask_gui->guipoints)
    {
      dt_masks_dynbuf_free(mask_gui->guipoints);
      dt_masks_dynbuf_free(mask_gui->guipoints_payload);
      mask_gui->guipoints = NULL;
      mask_gui->guipoints_payload = NULL;
      mask_gui->guipoints_count = 0;
    }

    // The visible form is the current unfinished shape while creation is active.
    // If it was never appended to develop->forms, drop it before selecting the
    // last completed shape from this creation session.
    if(!IS_NULL_PTR(temporary_form) && IS_NULL_PTR(dt_masks_get_from_id(dev, temporary_form->formid)))
    {
      dt_masks_set_visible_form(dev, NULL);
      dt_masks_free_form(temporary_form);
    }

    dt_masks_creation_mode_quit(mask_gui);
    g_list_free(mask_gui->creation_formids);
    mask_gui->creation_formids = NULL;
    mask_gui->creation_last_formid = 0;
    mask_gui->creation_type = DT_MASKS_NONE;
    mask_gui->creation_module = NULL;

    if(!IS_NULL_PTR(creation_module))
    {
      dt_masks_set_edit_mode(creation_module, DT_MASKS_EDIT_FULL);
      if(last_formid > 0)
      {
        // Keep the shape manager selection in sync without letting its selection
        // handler replace the visible module group with the standalone shape.
        dt_dev_masks_selection_change(dev, creation_module, last_formid, FALSE);
      }

      dt_iop_gui_blend_masks_update(creation_module);
      dt_iop_gui_blend_data_t *blend_data
          = creation_module->gui ? (dt_iop_gui_blend_data_t *)creation_module->gui->blend_data : NULL;
      if(!IS_NULL_PTR(dev) && !IS_NULL_PTR(dev->form_gui))
        dev->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
      if(!IS_NULL_PTR(blend_data) && GTK_IS_TOGGLE_BUTTON(blend_data->masks_edit))
      {
        // Creation mode keeps the edit button visually inactive while a shape
        // type button owns the interaction. Once creation is exited, restore both
        // the module edit state and the visible toggle explicitly.
        blend_data->masks_shown = DT_MASKS_EDIT_FULL;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_edit), TRUE);
        gtk_widget_queue_draw(blend_data->masks_edit);
      }

      if(last_formid > 0 && !IS_NULL_PTR(dev) && !IS_NULL_PTR(dev->form_gui))
      {
        dt_masks_form_gui_t *current_gui = dev->form_gui;
        dt_masks_form_t *visible_form = dt_masks_get_visible_form(dev);
        const int selected_index = dt_masks_group_index_from_formid(visible_form, last_formid);
        if(selected_index >= 0)
        {
          // The displayed overlay is the module group; select the last completed
          // form inside that group once creation is closed.
          current_gui->group_selected = selected_index;
          current_gui->form_selected = TRUE;
          // reset other variables
          current_gui->border_selected = FALSE;
          current_gui->source_selected = FALSE;
          current_gui->node_selected = FALSE;
          current_gui->handle_selected = FALSE;
          current_gui->seg_selected = FALSE;
          current_gui->handle_border_selected = FALSE;
          current_gui->node_selected_idx = -1;
          current_gui->form_dragging = FALSE;
          current_gui->source_dragging = FALSE;
          current_gui->form_rotating = FALSE;
          current_gui->pivot_selected = FALSE;
        }
      }
    }
    else if(last_formid > 0)
    {
      dt_masks_change_form_gui(dev, dt_masks_get_from_id(dev, last_formid));
      dt_dev_masks_selection_change(dev, NULL, last_formid, TRUE);
      if(!IS_NULL_PTR(dev) && !IS_NULL_PTR(dev->form_gui))
      {
        // A standalone visible form is rendered at index 0.
        dev->form_gui->group_selected = 0;
        dev->form_gui->form_selected = TRUE;
      }
    }
    else
    {
      dt_masks_change_form_gui(dev, NULL);
    }

    return TRUE;
  }
  return FALSE;
}

gboolean dt_masks_gui_remove(struct dt_iop_module_t *module, dt_masks_form_t *mask_form,
                             dt_masks_form_gui_t *mask_gui, const int parent_id)
{
  if(mask_gui->edit_mode != DT_MASKS_EDIT_FULL)
    return FALSE;

  // Just clean temp mask if we are in creation mode
  if(dt_masks_form_exit_creation(module, mask_gui))
    return TRUE;

  // we remove the selected node (and the entire form if there is too few nodes left)
  if(((mask_form->type & DT_MASKS_IS_PATH_SHAPE) != 0) && mask_gui->node_selected)
  {
    if(g_list_shorter_than(mask_form->points, 3))
      return _masks_remove_shape(module, mask_form, parent_id, mask_gui, mask_gui->group_selected);

    dt_masks_remove_node(module, mask_form, parent_id, mask_gui, mask_gui->group_selected,
                         mask_gui->node_hovered);

    return TRUE;
  }
  // we remove the entire shape
  else if(parent_id > 0)
  {
    dt_masks_remove_or_delete(module, mask_form, parent_id, mask_gui, mask_form->formid);
    return TRUE; // something happened even if the dialog was cancelled.
  }
  return FALSE;
}

void dt_masks_gui_form_remove(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui, int form_index)
{
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  mask_gui->geometry_generation = 0;
  mask_gui->formid = 0;

  if(!IS_NULL_PTR(gui_points))
  {
    gui_points->points_count = gui_points->border_count = gui_points->source_count = 0;
    gui_points->border_skip_count = 0;
    dt_pixelpipe_cache_free_align(gui_points->points);
    gui_points->points = NULL;
    dt_pixelpipe_cache_free_align(gui_points->border);
    gui_points->border = NULL;
    dt_pixelpipe_cache_free_align(gui_points->border_skips);
    gui_points->border_skips = NULL;
    dt_pixelpipe_cache_free_align(gui_points->source);
    gui_points->source = NULL;
  }
}

void dt_masks_gui_form_test_create(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui,
                                   dt_iop_module_t *module)
{
  // we test if the geometry the cached outlines were built against has moved
  const uint64_t live_generation = dt_geometry_chain_generation(mask_gui->dev->geometry_chain);
  if(dt_get_debug_flags() & DT_DEBUG_MASKS)
    dt_print(DT_DEBUG_MASKS, "[masks] outline cache: held for geometry %lu, live %lu -> %s\n",
             (unsigned long)mask_gui->geometry_generation, (unsigned long)live_generation,
             (mask_gui->geometry_generation == 0)
                 ? "REBUILD (nothing cached)"
                 : ((mask_gui->geometry_generation != live_generation) ? "REBUILD (geometry moved)" : "reuse"));

  if(mask_gui->geometry_generation != 0)
  {
    if(mask_gui->geometry_generation != live_generation)
    {
      mask_gui->geometry_generation = 0;
      mask_gui->formid = 0;
      g_list_free_full(mask_gui->points, dt_masks_form_gui_points_free);
      mask_gui->points = NULL;
    }
  }

  // we create the form if needed
  if(mask_gui->geometry_generation == 0)
  {
    if(mask_form->type & DT_MASKS_GROUP)
    {
      int form_index = 0;
      for(GList *group_node = mask_form->points; group_node; group_node = g_list_next(group_node))
      {
        dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
        dt_masks_form_t *child_form = dt_masks_get_from_id(mask_gui->dev, group_entry->formid);
        if(IS_NULL_PTR(child_form)) return;
        dt_masks_gui_form_create(child_form, mask_gui, form_index, module);
        form_index++;
      }
    }
    else
    {
      dt_masks_gui_form_create(mask_form, mask_gui, 0, module);
    }
  }
}

void dt_masks_append_form(dt_develop_t *develop, dt_masks_form_t *mask_form)
{
  // dev->forms is its own claim on the object, independent of dev->allforms's claim
  // (taken at creation, dt_masks_create_ext): take a reference for this list membership.
  dt_masks_form_ref(mask_form);
  dt_pthread_rwlock_wrlock(&develop->masks_mutex);
  develop->forms = g_list_append(develop->forms, mask_form);
  dt_pthread_rwlock_unlock(&develop->masks_mutex);
}

void dt_masks_remove_form(dt_develop_t *develop, dt_masks_form_t *mask_form)
{
  dt_pthread_rwlock_wrlock(&develop->masks_mutex);
  develop->forms = g_list_remove(develop->forms, mask_form);
  dt_pthread_rwlock_unlock(&develop->masks_mutex);
  // Release dev->forms's claim (see dt_masks_append_form). Does not necessarily free the
  // object: dev->allforms and/or history snapshots may still reference it.
  dt_masks_form_unref(mask_form);
}

void dt_masks_gui_form_save_creation(dt_develop_t *develop, dt_iop_module_t *module, dt_masks_form_t *mask_form,
                                     dt_masks_form_gui_t *mask_gui)
{
  // we check if the id is already registered
  _check_id(develop, mask_form);

  // mask nb will be at least the length of the list
  guint form_count = 0;

  // count only the same forms to have a clean numbering
  dt_pthread_rwlock_rdlock(&develop->masks_mutex);
  for(GList *form_node = develop->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
    if(existing_form->type == mask_form->type) form_count++;
  }
  dt_pthread_rwlock_unlock(&develop->masks_mutex);

  gboolean name_exists = FALSE;

  // check that we do not have duplicate, in case some masks have been
  // removed we can have hole and so nb could already exists.
  do
  {
    name_exists = FALSE;
    form_count++;

    if(mask_form->functions && mask_form->functions->set_form_name)
      mask_form->functions->set_form_name(mask_form, form_count);

    dt_pthread_rwlock_rdlock(&develop->masks_mutex);
    for(GList *form_node = develop->forms; form_node; form_node = g_list_next(form_node))
    {
      dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
      if(!strcmp(existing_form->name, mask_form->name))
      {
        name_exists = TRUE;
        break;
      }
    }
    dt_pthread_rwlock_unlock(&develop->masks_mutex);

  } while(name_exists);

  dt_masks_form_update_gravity_center(develop, mask_form);

  dt_masks_form_group_t *group_entry = NULL;
  if(!IS_NULL_PTR(module))
  {
    group_entry = malloc(sizeof(dt_masks_form_group_t));
    if(IS_NULL_PTR(group_entry)) return;
  }

  dt_masks_append_form(develop, mask_form);

  if(!IS_NULL_PTR(module))
  {
    // is there already a masks group for this module ?
    dt_masks_form_t *group_form = _group_from_module(develop, module);
    if(IS_NULL_PTR(group_form))
    {
      // we create a new group
      if(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        group_form = _group_create(develop, module, DT_MASKS_GROUP | DT_MASKS_CLONE);
      else
        group_form = _group_create(develop, module, DT_MASKS_GROUP);
    }
    group_form = dt_masks_cow_touch(develop, group_form);
    // we add the form in this group
    group_entry->formid = mask_form->formid;
    group_entry->parentid = group_form->formid;
    group_entry->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION;
    group_entry->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    group_form->points = g_list_append(group_form->points, group_entry);
    
    // we update module gui

    if(IS_NULL_PTR(mask_gui)) dt_iop_gui_blend_masks_update(module);
    dt_dev_add_history_item(develop, module, TRUE, TRUE);
  }

  if(!IS_NULL_PTR(mask_gui))
  {
    mask_gui->creation_formids = g_list_append(mask_gui->creation_formids, GINT_TO_POINTER(mask_form->formid));
    mask_gui->creation_last_formid = mask_form->formid;

    if(!IS_NULL_PTR(module))
    {
      DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, group_entry->formid,
                                    group_entry->parentid, DT_MASKS_EVENT_ADD);
    }
    else
    {
      DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, mask_form->formid,
                                    0, DT_MASKS_EVENT_ADD);
    }

    // Keep creation mode active. The saved form remains in develop->forms for
    // session rendering, while the visible form becomes the next unfinished
    // shape so mouse events still target the creation preview.
    dt_masks_form_t *next_form = dt_masks_create(mask_gui->creation_type);
    if(IS_NULL_PTR(next_form))
    {
      dt_masks_form_exit_creation(module, mask_gui);
      return;
    }

    g_list_free_full(mask_gui->points, dt_masks_form_gui_points_free);
    mask_gui->points = NULL;
    dt_masks_dynbuf_free(mask_gui->guipoints);
    mask_gui->guipoints = NULL;
    dt_masks_dynbuf_free(mask_gui->guipoints_payload);
    mask_gui->guipoints_payload = NULL;
    mask_gui->guipoints_count = 0;
    mask_gui->geometry_generation = 0;
    mask_gui->formid = 0;
    mask_gui->creation_closing_form = FALSE;
    dt_masks_soft_reset_form_gui(mask_gui);
    dt_masks_set_visible_form(develop, next_form);
    if(!IS_NULL_PTR(module)) dt_iop_gui_blend_masks_update(module);
  }
}

int dt_masks_events_mouse_leave(struct dt_iop_module_t *module)
{
  return 0;
}

int dt_masks_events_mouse_enter(struct dt_iop_module_t *module)
{
  return 0;
}

gboolean dt_masks_is_anything_selected(const dt_masks_form_gui_t *mask_gui)
{
  return mask_gui->form_selected
          || mask_gui->source_selected
          || mask_gui->seg_selected
          || mask_gui->node_selected
          || mask_gui->handle_selected
          || mask_gui->handle_border_selected;
}

gboolean dt_masks_is_anything_hovered(const dt_masks_form_gui_t *mask_gui)
{
  return mask_gui->node_hovered >= 0
          || mask_gui->handle_hovered >= 0
          || mask_gui->handle_border_hovered >= 0
          || mask_gui->seg_hovered >= 0;
}

static void _set_cursor_shape(dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(mask_gui)) return;

  if(mask_gui->creation)
  {
    dt_masks_form_t *creation_form = dt_masks_get_visible_form(mask_gui->dev);
    if(!IS_NULL_PTR(creation_form) && (creation_form->type & DT_MASKS_BRUSH))
    {
      // The brush tool draws its own filled size/fading preview circle at the cursor
      // position (_brush_events_post_expose) -- a system cursor on top of it is redundant.
      dt_control_set_cursor_visible(FALSE);
      return;
    }
  }

  // Any other case un-hides the cursor: it may have been hidden by a brush creation that
  // just ended or was switched away from.
  dt_control_set_cursor_visible(TRUE);

  // circular arrows
  if(mask_gui->pivot_selected)
    dt_control_queue_cursor(GDK_EXCHANGE);
  // pointing hand
  else if(mask_gui->creation_closing_form)
    dt_control_queue_cursor(GDK_HAND2);
  // precise-placement cursor while drawing a new shape -- distinct from darkroom's own
  // default "dot"/"crosshair" cursors (views/darkroom.c's _darkroom_set_default_cursor)
  else if(mask_gui->creation)
    dt_control_queue_cursor_by_name("cell");

  /*else if(gui->handle_dragging >= 0)
    dt_control_set_cursor(GDK_HAND1);*/

  // crosshair
  else if(dt_masks_is_anything_selected(mask_gui) || dt_masks_is_anything_hovered(mask_gui))
    dt_control_queue_cursor(GDK_FLEUR);
}

static void _apply_gui_button_pressed_state(dt_masks_form_gui_t *mask_gui, const int button,
                                            const uint32_t state,
                                            const gboolean shape_was_selected)
{
  if(IS_NULL_PTR(mask_gui) || mask_gui->creation || button != 1) return;
  // Drag is only allowed when this click happens on a shape that was already selected.
  // We still rebuild the fine-grained selection from the current hover target first, so the
  // pressed node/handle/segment becomes the active drag target when dragging is allowed.
  const gboolean prev_node_selected = mask_gui->node_selected;
  const int prev_node_selected_idx = mask_gui->node_selected_idx;
  const gboolean prev_form_selected = mask_gui->form_selected;
  const gboolean prev_border_selected = mask_gui->border_selected;
  const gboolean prev_source_selected = mask_gui->source_selected;

  mask_gui->node_selected = FALSE;
  mask_gui->handle_selected = FALSE;
  mask_gui->handle_border_selected = FALSE;
  mask_gui->seg_selected = FALSE;
  mask_gui->node_selected_idx = -1;
  mask_gui->form_selected = FALSE;
  mask_gui->border_selected = FALSE;
  mask_gui->source_selected = FALSE;

  if(mask_gui->node_hovered >= 0)
  {
    mask_gui->node_selected = TRUE;
    mask_gui->node_selected_idx = mask_gui->node_hovered;
  }
  else if(mask_gui->handle_hovered >= 0)
  {
    if(prev_node_selected)
    {
      mask_gui->node_selected = TRUE;
      mask_gui->node_selected_idx = prev_node_selected_idx;
    }
    mask_gui->handle_selected = TRUE;
  }
  else if(mask_gui->handle_border_hovered >= 0)
  {
    if(prev_node_selected)
    {
      mask_gui->node_selected = TRUE;
      mask_gui->node_selected_idx = prev_node_selected_idx;
    }
    mask_gui->handle_border_selected = TRUE;
  }
  else if(mask_gui->seg_hovered >= 0)
  {
    mask_gui->seg_selected = TRUE;
  }
  else
  {
    mask_gui->form_selected = prev_form_selected;
    mask_gui->border_selected = prev_border_selected;
    mask_gui->source_selected = prev_source_selected;
  }

  if(mask_gui->form_rotating || mask_gui->border_toggling || mask_gui->gradient_toggling) return;
  if(dt_modifier_is(state, DT_PRIMARY_MASK)) return;
  if(!shape_was_selected) return;

  dt_masks_gui_set_dragging(mask_gui);
}

/**
 * @brief Convert the GTK/Cairo widget cursor once for the full mask event chain.
 *
 * The event entry points are the only place where widget-space `x, y` are consumed.
 * Downstream handlers reuse the cached positions:
 * - `mask_gui->rel_pos`: normalized output-image coordinates
 * - `mask_gui->pos`: absolute output-image coordinates
 * - `mask_gui->raw_pos`: absolute raw input-image coordinates
 */
static void _dt_masks_events_set_current_pos(const double x, const double y, dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(mask_gui)) return;

  float point[2] = { (float)x, (float)y };
  dt_dev_coordinates_widget_to_image_norm(mask_gui->dev, point, 1);
  mask_gui->rel_pos[0] = point[0];
  mask_gui->rel_pos[1] = point[1];

  dt_dev_coordinates_image_norm_to_image_abs(mask_gui->dev, point, 1);
  mask_gui->pos[0] = point[0];
  mask_gui->pos[1] = point[1];

  mask_gui->raw_pos[0] = point[0];
  mask_gui->raw_pos[1] = point[1];
  dt_dev_coordinates_image_abs_to_raw_abs(mask_gui->dev, mask_gui->raw_pos, 1);
}

static int _dt_masks_events_mouse_moved(dt_develop_t *dev, struct dt_iop_module_t *module, double x,
                                       double y, double pressure, int which);

int dt_masks_events_mouse_moved(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y, double pressure, int which)
{
  /* Timed because it is the other thing that scales with the number of shapes and is not part of
   * the expose: hit-testing walks every shape in the group and, for a brush, every point of it.
   * The #1158 logs show the darkroom redraw growing to 400 ms with only 1.5 ms of mask overlay in
   * it, so whatever grows is either here or in the expose's earlier stages -- both are now timed. */
  dt_times_t moved_start = { 0 };
  dt_get_times(&moved_start);
  const int moved_result = _dt_masks_events_mouse_moved(dev, module, x, y, pressure, which);
  if(dt_get_debug_flags() & DT_DEBUG_PERF) dt_show_times(&moved_start, "[masks] mouse moved");
  return moved_result;
}

static int _dt_masks_events_mouse_moved(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y, double pressure, int which)
{
  // This assume that if this event is generated, the mouse is over the center window.
  // record mouse position even if there are no masks visible
  dt_masks_form_gui_t *mask_gui = dev->form_gui;
  dt_masks_form_t *mask_form = dt_masks_get_visible_form(dev);
  if(IS_NULL_PTR(mask_gui)) return 0;
  _dt_masks_events_set_current_pos(x, y, mask_gui);

  // do not process if no forms visible
  if(IS_NULL_PTR(mask_form)) return 0;

  // add an option to allow skip mouse events while editing masks
  if(dev->darkroom_skip_mouse_events) return 0;

  int result = 0;
  if((mask_form->type & DT_MASKS_GROUP) && _dt_masks_events_group_blocks_motion(mask_gui))
  {
    result = 1;
  }
  else
  {
    dt_masks_form_group_t *group_entry = NULL;
    int parent_id = 0;
    int form_index = 0;
    dt_masks_form_t *dispatch_form
        = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);
    dispatch_form = dt_masks_cow_touch(dev, dispatch_form);

    if(_dt_masks_events_should_update_hover_on_move(mask_gui))
      result = _dt_masks_events_update_hover(dispatch_form, mask_gui, form_index);

    if(!result && dispatch_form && dispatch_form->functions && dispatch_form->functions->mouse_moved)
      result = dispatch_form->functions->mouse_moved(module, x, y, pressure, which,
                                                     dispatch_form, parent_id, mask_gui, form_index);
  }

  if(!IS_NULL_PTR(mask_gui))
  {
    // Re-read the visible form. dt_masks_cow_touch() above may have cloned it, spliced the
    // clone into dev->forms in place of the original, re-pointed form_gui->form_visible at
    // the clone and dropped the original's last reference -- freeing it. mask_form was
    // captured BEFORE that call, so it can be dangling here; form_visible is the pointer the
    // COW maintains.
    //
    // Sentry 143237622: SIGSEGV inside g_list_length() walking the freed points list, via
    // _set_hinter_message() -> _polygon_set_hint_message(), on an ordinary darkroom
    // mouse-move over a polygon.
    mask_form = dt_masks_get_visible_form(dev);
    if(!IS_NULL_PTR(mask_form)) _set_hinter_message(mask_gui, mask_form);
    _set_cursor_shape(mask_gui);
  }
  return result;
}

int dt_masks_events_button_released(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y, int button,
                                    uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(dev->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *mask_form = dt_masks_get_visible_form(dev);
  dt_masks_form_gui_t *mask_gui = dev->form_gui;
  if(IS_NULL_PTR(mask_gui)) return 0;
  _dt_masks_events_set_current_pos(x, y, mask_gui);
  if(IS_NULL_PTR(mask_form)) return 0;

  dt_masks_form_group_t *group_entry = NULL;
  int parent_id = 0;
  int form_index = 0;
  dt_masks_form_t *dispatch_form
      = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);
  dispatch_form = dt_masks_cow_touch(dev, dispatch_form);

  // dt_masks_cow_touch() above may have cloned the visible form, spliced the clone into
  // dev->forms in place of the original, re-pointed form_gui->form_visible at it and
  // dropped the original's last reference -- freeing it. mask_form was captured before
  // that call, so re-read what form_visible points at now rather than using a pointer
  // that may already be dangling. Same defect as Sentry 143237622 in mouse_moved.
  mask_form = dt_masks_get_visible_form(dev);

  int result = 0;
  if(!IS_NULL_PTR(dispatch_form) && dispatch_form->functions && dispatch_form->functions->button_released)
    result = dispatch_form->functions->button_released(module, x, y, button,
                                                       state, dispatch_form, parent_id, mask_gui, form_index);

  if(_dt_masks_events_flush_rebuild_if_needed(module, dispatch_form, mask_gui, form_index, button))
    result = 1;

  if(!IS_NULL_PTR(mask_form) && (mask_form->type & DT_MASKS_GROUP) && !IS_NULL_PTR(mask_gui))
  {
    const dt_masks_form_group_t *selected_group_entry
        = dt_masks_form_get_selected_group_live(mask_form, mask_gui);
    if(selected_group_entry)
      dt_dev_masks_selection_change(dev, module,
                                    selected_group_entry->formid, FALSE);
  }

  if(mask_gui && !mask_gui->creation && button == 1)
    dt_masks_gui_reset_dragging(mask_gui);

  if(!IS_NULL_PTR(mask_gui))
  {
    _set_hinter_message(mask_gui, mask_form);
    _set_cursor_shape(mask_gui);
  }

  return result;
}

int dt_masks_events_button_pressed(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y, double pressure,
                                   int button, int event_type, uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(dev->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *mask_form = dt_masks_get_visible_form(dev);
  dt_masks_form_gui_t *mask_gui = dev->form_gui;
  if(IS_NULL_PTR(mask_gui)) return 0;

  _dt_masks_events_set_current_pos(x, y, mask_gui);
  if(IS_NULL_PTR(mask_form)) return 0;
  const gboolean prev_any_selected = dt_masks_is_anything_selected(mask_gui);
  const int prev_group_selected = mask_gui->group_selected;

  /*DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_SELECTION_CHANGED, NULL, NULL);*/

  if(mask_form->type & DT_MASKS_GROUP)
    _dt_masks_events_group_update_selection(mask_form, mask_gui);

  dt_masks_form_group_t *group_entry = NULL;
  int parent_id = 0;
  int form_index = 0;
  dt_masks_form_t *dispatch_form
      = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);
  dispatch_form = dt_masks_cow_touch(dev, dispatch_form);

  // dt_masks_cow_touch() above may have cloned the visible form, spliced the clone into
  // dev->forms in place of the original, re-pointed form_gui->form_visible at it and
  // dropped the original's last reference -- freeing it. mask_form was captured before
  // that call, so re-read what form_visible points at now rather than using a pointer
  // that may already be dangling. Same defect as Sentry 143237622 in mouse_moved.
  mask_form = dt_masks_get_visible_form(dev);
  _dt_masks_events_update_hover(dispatch_form, mask_gui, form_index);

  gboolean return_val = FALSE;
  if(!IS_NULL_PTR(dispatch_form) && dispatch_form->functions && dispatch_form->functions->button_pressed)
    return_val = dispatch_form->functions->button_pressed(module, x, y, pressure,
                                                          button, event_type, state,
                                                          dispatch_form, parent_id, mask_gui, form_index);
  // Throw a selection change event.
  // `dispatch_form` can pass NULL in case of deselection.
  dt_masks_select_form(dev, module, dispatch_form);

  const gboolean shape_was_selected = (mask_form->type & DT_MASKS_GROUP)
                                          ? (prev_group_selected >= 0 && prev_group_selected == form_index)
                                          : prev_any_selected;
  _apply_gui_button_pressed_state(mask_gui, button, state, shape_was_selected);

  // Refresh hover/highlight state, the hint message and the cursor shape now that the click
  // has been dispatched: button_pressed above may have changed geometry (e.g. a new node in
  // creation mode) or the active selection, neither of which the pre-dispatch hover computed
  // at the top of this function accounts for. Without this, the display only catches up with
  // what the click just did on the next physical mouse move -- same class of staleness as
  // mouse_moved()/button_released(), which already refresh all three at their own tail.
  _dt_masks_events_update_hover(dispatch_form, mask_gui, form_index);
  _set_hinter_message(mask_gui, mask_form);
  _set_cursor_shape(mask_gui);

  if(button == 3 && !return_val)
  {
    // mouse is over a form or one of its handles/nodes
    if(!IS_NULL_PTR(dispatch_form) && (mask_gui->creation
                         || dt_masks_is_anything_hovered(mask_gui)
                         || dt_masks_is_anything_selected(mask_gui)))
    {
      GtkWidget *menu = dt_masks_create_menu(mask_gui, dispatch_form, group_entry,
                                             mask_gui->rel_pos[0], mask_gui->rel_pos[1]);
      if(!IS_NULL_PTR(menu))
      {
        gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
        return_val = TRUE;
      }
    }
  }

  return return_val;
}

int dt_masks_events_key_pressed(dt_develop_t *dev, struct dt_iop_module_t *module, GdkEventKey *event)
{
  dt_masks_form_t *mask_form = dt_masks_get_visible_form(dev);
  if(IS_NULL_PTR(mask_form)) return 0;
  dt_masks_form_gui_t *mask_gui = dev->form_gui;
  if(IS_NULL_PTR(mask_gui)) return 0;

  gboolean return_value = FALSE;
  if(mask_form->type & DT_MASKS_GROUP)
  {
    dt_masks_form_group_t *group_entry = NULL;
    int parent_id = 0;
    int form_index = 0;
    dt_masks_form_t *dispatch_form
        = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);
    dispatch_form = dt_masks_cow_touch(dev, dispatch_form);

    // dt_masks_cow_touch() above may have cloned the visible form, spliced the clone into
    // dev->forms in place of the original, re-pointed form_gui->form_visible at it and
    // dropped the original's last reference -- freeing it. mask_form was captured before
    // that call, so re-read what form_visible points at now rather than using a pointer
    // that may already be dangling. Same defect as Sentry 143237622 in mouse_moved.
    mask_form = dt_masks_get_visible_form(dev);
    if(dispatch_form && dispatch_form->functions && dispatch_form->functions->key_pressed)
      return_value = dispatch_form->functions->key_pressed(module, event, dispatch_form,
                                                           parent_id, mask_gui, form_index);

    if(!return_value && mask_form->functions->key_pressed)
    {
      mask_form = dt_masks_cow_touch(dev, mask_form);
      return_value = mask_form->functions->key_pressed(module, event, mask_form, 0, mask_gui, 0);
    }
  }
  else if(mask_form->functions->key_pressed)
  {
    mask_form = dt_masks_cow_touch(dev, mask_form);
    return_value = mask_form->functions->key_pressed(module, event, mask_form, 0, mask_gui, 0);
  }
  
  if(!return_value)
  {
    guint key = dt_keys_mainpad_alternatives(event->keyval);
    switch(key)
    {
      case GDK_KEY_Escape:
      {
        return_value = dt_masks_form_exit_creation(module, mask_gui);
        break;
      }
      case GDK_KEY_Delete:
      {
        if(mask_gui->group_selected >= 0)
        {
          // Delete shape from current group
          dt_masks_form_group_t *selected_group = dt_masks_form_get_selected_group(mask_form, mask_gui);
          if(IS_NULL_PTR(selected_group)) return 0;
          dt_masks_form_t *selected_form = dt_masks_get_from_id(dev, selected_group->formid);
          if(selected_form)
            return_value = dt_masks_gui_remove(module, selected_form, mask_gui, selected_group->parentid);
          break;
        }
      }
    }
  }

  return return_value;
}

/* One conf key per wheel row. The mapping is application-wide on purpose: which property the
 * wheel edits is a user habit, not a property of a shape or of the module the mask belongs to. */
static const char *const _scroll_conf_keys[DT_MASKS_SCROLL_MODIFIER_LAST]
    = { "plugins/darkroom/masks/scroll/plain",
        "plugins/darkroom/masks/scroll/shift",
        "plugins/darkroom/masks/scroll/primary",
        "plugins/darkroom/masks/scroll/primary_shift" };

/* Written verbatim into the user's conf file, so these are a storage format: never translate
 * them, never reorder them against dt_masks_interaction_t, and keep them in sync with the enum
 * declared for these four keys in data/anselconfig.xml.in. */
static const char *const _interaction_conf_values[DT_MASKS_INTERACTION_LAST]
    = { "none", "size", "fading", "opacity", "rotation" };

/* Both enums declare only non-negative enumerators, so a compiler is free to give them an
 * unsigned underlying type -- clang does, and then `x < 0' is a tautology it rejects under
 * -Weverything -Werror. Casting to unsigned instead of dropping the lower bound keeps the
 * check honest either way: a negative value converts to a huge unsigned one and is caught by
 * the upper bound, whatever signedness the compiler picked. */
dt_masks_interaction_t dt_masks_scroll_mapping_get(dt_masks_scroll_modifier_t modifier)
{
  if((unsigned int)modifier >= DT_MASKS_SCROLL_MODIFIER_LAST) return DT_MASKS_INTERACTION_UNDEF;

  const char *value = dt_conf_get_string_const(_scroll_conf_keys[modifier]);
  if(IS_NULL_PTR(value)) return DT_MASKS_INTERACTION_UNDEF;

  for(int i = 0; i < DT_MASKS_INTERACTION_LAST; i++)
    if(!strcmp(value, _interaction_conf_values[i])) return (dt_masks_interaction_t)i;

  return DT_MASKS_INTERACTION_UNDEF;
}

void dt_masks_scroll_mapping_set(dt_masks_scroll_modifier_t modifier, dt_masks_interaction_t interaction)
{
  if((unsigned int)modifier >= DT_MASKS_SCROLL_MODIFIER_LAST) return;
  if((unsigned int)interaction >= DT_MASKS_INTERACTION_LAST)
    interaction = DT_MASKS_INTERACTION_UNDEF;

  dt_conf_set_string(_scroll_conf_keys[modifier], _interaction_conf_values[interaction]);
}

const char *dt_masks_scroll_modifier_name(dt_masks_scroll_modifier_t modifier)
{
  switch(modifier)
  {
    case DT_MASKS_SCROLL_SHIFT:
      return N_("Shift+Scroll");
    case DT_MASKS_SCROLL_PRIMARY:
      return N_("Ctrl+Scroll");
    case DT_MASKS_SCROLL_PRIMARY_SHIFT:
      return N_("Ctrl+Shift+Scroll");
    case DT_MASKS_SCROLL_PLAIN:
    default:
      return N_("Scroll");
  }
}

const char *dt_masks_interaction_name(dt_masks_interaction_t interaction)
{
  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
      return N_("Size");
    case DT_MASKS_INTERACTION_FADING:
      return N_("Fading");
    case DT_MASKS_INTERACTION_OPACITY:
      return N_("Opacity");
    case DT_MASKS_INTERACTION_ROTATION:
      return N_("Rotation");
    case DT_MASKS_INTERACTION_UNDEF:
    default:
      return N_("Nothing");
  }
}

const char *dt_masks_interaction_alias_name(dt_masks_interaction_t interaction)
{
  switch(interaction)
  {
    case DT_MASKS_INTERACTION_FADING:
      return N_("Curvature");
    default:
      // Size, opacity and rotation mean the same thing to every shape, and "nothing" is not
      // a property.
      return NULL;
  }
}

dt_masks_interaction_t dt_masks_scroll_get_interaction(uint32_t key_state)
{
  const GdkModifierType state = (GdkModifierType)key_state;
  dt_masks_scroll_modifier_t modifier;

  // Most specific first: ctrl+shift also satisfies neither of the single-modifier tests, but
  // reading it last would leave the combination unreachable if that ever stopped being true.
  if(dt_modifier_is(state, GDK_SHIFT_MASK | DT_PRIMARY_MASK))
    modifier = DT_MASKS_SCROLL_PRIMARY_SHIFT;
  else if(dt_modifier_is(state, DT_PRIMARY_MASK))
    modifier = DT_MASKS_SCROLL_PRIMARY;
  else if(dt_modifier_is(state, GDK_SHIFT_MASK))
    modifier = DT_MASKS_SCROLL_SHIFT;
  else if(dt_modifier_is(state, 0))
    modifier = DT_MASKS_SCROLL_PLAIN;
  else
    // A combination the mapping does not cover (alt+scroll, ...): not ours to interpret.
    return DT_MASKS_INTERACTION_UNDEF;

  return dt_masks_scroll_mapping_get(modifier);
}

int dt_masks_events_mouse_scrolled(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y,
                                   int scroll_up, uint32_t key_state, int scrolling_delta)
{
  // add an option to allow skip mouse events while editing masks
  if(dev->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *mask_form = dt_masks_get_visible_form(dev);
  dt_masks_form_gui_t *mask_gui = dev->form_gui;
  if(IS_NULL_PTR(mask_gui)) return 0;

  _dt_masks_events_set_current_pos(x, y, mask_gui);
  if(IS_NULL_PTR(mask_form)) return 0;

  int result = 0;
  const gboolean scroll_increases = dt_mask_scroll_increases(scroll_up);

  // we want delta_y to be an absolute scrolling speed
  int scroll_flow = (scrolling_delta < 0) ? -scrolling_delta : scrolling_delta;

  dt_masks_form_group_t *group_entry = NULL;
  int parent_id = 0;
  int form_index = 0;
  dt_masks_form_t *dispatch_form
      = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);
  dispatch_form = dt_masks_cow_touch(dev, dispatch_form);

  // dt_masks_cow_touch() above may have cloned the visible form, spliced the clone into
  // dev->forms in place of the original, re-pointed form_gui->form_visible at it and
  // dropped the original's last reference -- freeing it. mask_form was captured before
  // that call, so re-read what form_visible points at now rather than using a pointer
  // that may already be dangling. Same defect as Sentry 143237622 in mouse_moved.
  mask_form = dt_masks_get_visible_form(dev);

  if(!mask_gui->creation && !dt_masks_is_anything_selected(mask_gui))
    return 0;

  _dt_masks_events_update_hover(dispatch_form, mask_gui, form_index);

  if(!mask_gui->creation && !_dt_masks_events_cursor_over_form(dispatch_form, mask_gui, form_index))
    return 0;

  // Resolved once, here: shapes act on a named property and never read key modifiers.
  const dt_masks_interaction_t interaction = dt_masks_scroll_get_interaction(key_state);

  if(dispatch_form && dispatch_form->functions && dispatch_form->functions->mouse_scrolled
     && interaction != DT_MASKS_INTERACTION_UNDEF)
    result = dispatch_form->functions->mouse_scrolled(module, x, y,
                                                      scroll_increases ? 1 : 0, scroll_flow,
                                                      key_state, dispatch_form, parent_id, mask_gui, form_index,
                                                      interaction);

  if(!IS_NULL_PTR(mask_gui))
  {
    const gboolean hinted = _set_hinter_message(mask_gui, mask_form);
    dt_print(DT_DEBUG_INPUT,
             "[masks] scroll: ret=%d hinted=%d form=%p type=%d gui=%p group_selected=%d flow=%d state=0x%x\n",
             result, hinted, (void *)mask_form, mask_form ? mask_form->type : -1, (void *)mask_gui,
             mask_gui->group_selected, scroll_flow, key_state);
    if(hinted)
      result = 1;
  }
  return result;
}

gboolean dt_masks_node_is_cusp(const dt_masks_form_gui_points_t *gui_points, const int node_index)
{
  if(IS_NULL_PTR(gui_points) || IS_NULL_PTR(gui_points->points)) return FALSE;
  if(gui_points->points_count <= 0 || node_index < 0 || node_index >= gui_points->points_count) return FALSE;

  const float *point_values = &gui_points->points[node_index * 6];
  return (point_values[0 + 2] == point_values[2 + 2]
       && point_values[1 + 2] == point_values[3 + 2]);
}

/**
 * @brief Find the best attachment point on the shape contour for a ray crossing the form
 * 
 * The best point is the one with the smallest positive projection along the ray.
 * The result is offset from the contour by a given distance along the ray axis,
 * oriented toward the center of the ray segment [ray_2, ray_1].
 *  
 * @param ray_1 First point of the ray
 * @param ray_2 Second point of the ray
 * @param points Array of points defining the shape contour
 * @param points_count Number of points in the contour
 * @param first_pt Index of the first point to consider
 * @param is_closed_shape Whether the contour is closed
 * @param result Array to store the resulting attachment point
 */
static void _dt_masks_find_best_attachment_point(const float ray_1[2], const float ray_2[2],
                                                 const float *points, const int points_count, const float zoom_scale,
                                                 const int first_pt,
                                                 const gboolean is_closed_shape,
                                                 const float offset_factor,
                                                 float result[2])
{
  // Fallback: no intersection found.
  result[0] = ray_1[0];
  result[1] = ray_1[1];

  const int available_points = points_count - first_pt;
  if(available_points < 2) return;

  const float ray2_x = ray_2[0];
  const float ray2_y = ray_2[1];
  const float ray_center_x = 0.5f * (ray_1[0] + ray_2[0]);
  const float ray_center_y = 0.5f * (ray_1[1] + ray_2[1]);
  const float dir_x = ray_1[0] - ray_2[0];
  const float dir_y = ray_1[1] - ray_2[1];
  float min_s = FLT_MAX;
  const float offset = DT_PIXEL_APPLY_DPI(12.0f * offset_factor) / zoom_scale;
  const float inv_dir_len = f_inv_sqrtf(dir_x * dir_x + dir_y * dir_y);
  const float ux = dir_x * inv_dir_len;
  const float uy = dir_y * inv_dir_len;


  const int segment_count = (available_points - 1) + ((is_closed_shape) ? 1 : 0);
  for(int seg = 0; seg < segment_count; seg++)
  {
    // Get the current segment [i, j], with wrap-around if the shape is closed.
    const int i = first_pt + seg;
    const int j = (i + 1 < points_count) ? (i + 1) : first_pt;
    const float x3 = points[i * 2];
    const float y3 = points[i * 2 + 1];
    const float x4 = points[j * 2];
    const float y4 = points[j * 2 + 1];

    // Compute the intersection of the ray with the segment.
    const float dx = x4 - x3;
    const float dy = y4 - y3;
    const float det = dx * (-dir_y) + dy * dir_x;
    if(det > -1e-8f && det < 1e-8f) continue;
    const float inv_det = 1.0f / det;

    const float segment_param = ((ray2_x - x3) * (-dir_y) + (ray2_y - y3) * dir_x) * inv_det;
    const float ray_param = ((x3 - ray2_x) * dy - (y3 - ray2_y) * dx) * inv_det;
    if(segment_param < 0.0f || segment_param > 1.0f || ray_param <= 0.0f || ray_param >= min_s) continue;

    min_s = ray_param;
    const float ix = ray2_x + ray_param * dir_x;
    const float iy = ray2_y + ray_param * dir_y;

    // Offset along the ray axis toward the ray segment center.
    const float to_center = (ray_center_x - ix) * ux + (ray_center_y - iy) * uy;
    const float side_sign = (to_center >= 0.0f) ? 1.0f : -1.0f;
    result[0] = ix + side_sign * offset * ux;
    result[1] = iy + side_sign * offset * uy;
  }
}

void dt_masks_draw_source(cairo_t *cr, dt_masks_form_gui_t *mask_gui, const int form_index,
                          const int node_count, const float zoom_scale,
                          struct dt_masks_gui_center_point_t *center_point,
                          const shape_draw_function_t *draw_shape_func)
{
  if(IS_NULL_PTR(mask_gui)) return;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(IS_NULL_PTR(gui_points)) return;

  if(!mask_gui->creation)
  {
    const float main[2] = { center_point->main.x, center_point->main.y };
    const float source[2] = { center_point->source.x, center_point->source.y };
    const gboolean is_open_shape = (mask_gui->type & DT_MASKS_IS_OPEN_SHAPE) != 0;
    const gboolean is_closed_shape = (mask_gui->type & DT_MASKS_IS_CLOSED_SHAPE) != 0;

    int first_point_index = 2;
    if((mask_gui->type & DT_MASKS_ELLIPSE) != 0)
      first_point_index = 10;
    else if((mask_gui->type & DT_MASKS_IS_PATH_SHAPE) != 0)
      first_point_index = node_count * 3;

    int attach_points_count = gui_points->points_count;
    int attach_source_count = gui_points->source_count;
    if((mask_gui->type & DT_MASKS_BRUSH) != 0)
    {
      attach_points_count /= 2;
      attach_source_count /= 2;
    }

    float head[2] = { 0.0f, 0.0f };
    float tail[2] = { 0.0f, 0.0f };

    // Find attachment point for the arrow's head with the main shape
    _dt_masks_find_best_attachment_point(main, source, gui_points->points, attach_points_count,
                                         zoom_scale, first_point_index, is_closed_shape, 1.f, head);

    // Find attachment point for the arrow's base with the source shape
    _dt_masks_find_best_attachment_point(source, main, gui_points->source, attach_source_count,
                                         zoom_scale, first_point_index, is_closed_shape, 0.5f, tail);

    const gboolean selected = (mask_gui->group_selected == form_index)
                              && (mask_gui->source_selected || mask_gui->source_dragging);
    gboolean draw_tail = TRUE;

    // Do not draw the arrow tail if the shape overlapes source,
    // Just draw the head pointing to the center of the source shape
    // and displace it at mid-distance between main and source center.
    // Open shapes always draw the tail since they have not really filled area.
    if(is_closed_shape)
    {
      // From more frequent to least frequent, to get out of loop earlier. Tail first, then source.
      const float pts[4] = { tail[0], tail[1], source[0], source[1] };
      gboolean overlap = (dt_masks_point_in_form_exact(pts, 2, gui_points->points, first_point_index,
                                                       gui_points->points_count, NULL, 0) >= 0);
      // Skip the second containment test when overlap is already detected.
      if(!overlap)
      {
        const float origin_pt[2] = { main[0], main[1] };
        overlap = (dt_masks_point_in_form_exact(origin_pt, 1, gui_points->source, first_point_index,
                                                gui_points->source_count, NULL, 0) >= 0);
      }

      // Update head position to be between main and source center point.
      if(overlap)
      {
        head[0] = 0.5f * (main[0] + source[0]);
        head[1] = 0.5f * (main[1] + source[1]);
      }

      const float arrow_len_sq = sqf(tail[0] - head[0]) + sqf(tail[1] - head[1]);
      draw_tail = arrow_len_sq > 1e-6f && !overlap;
    }

    // Calculate the angle, so the arrow head always points in the direction of the main shape's center.
    const float angle = is_open_shape ? atan2f(tail[1] - head[1], tail[0] - head[0])
                                      : atan2f(head[1] - main[1], head[0] - main[0]);

    dt_draw_arrow(cr, zoom_scale, selected, draw_tail, DT_MASKS_DASH_ROUND, head, tail, angle);



    
    if(dt_get_debug_flags() & DT_DEBUG_MASKS)
    {
      // Debug: show the main and source gravity points, show head and tail points
      cairo_save(cr);
      cairo_arc(cr, main[0], main[1], DT_PIXEL_APPLY_DPI(4.0f) / zoom_scale, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 1);
      cairo_fill(cr);
      cairo_arc(cr, source[0], source[1], DT_PIXEL_APPLY_DPI(4.0f) / zoom_scale, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 1);
      cairo_fill(cr);

      cairo_arc(cr, head[0], head[1], DT_PIXEL_APPLY_DPI(4.0f) / zoom_scale, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 1);
      cairo_fill(cr);
      cairo_arc(cr, tail[0], tail[1], DT_PIXEL_APPLY_DPI(4.0f) / zoom_scale, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
    }
  }

  // draw the source shape
  // Trick to draw the current polygon shape lines while editing, but draw the complete shape in all other cases
  const int rendered_node_count = node_count + !mask_gui->creation;
  const gboolean shape_selected = (mask_gui->group_selected == form_index)
                                  && (mask_gui->form_selected || mask_gui->form_dragging);

  dt_draw_source_shape(mask_gui->dev, cr, zoom_scale, shape_selected, gui_points->source, gui_points->source_count,
                       rendered_node_count, draw_shape_func);
  
}

void dt_masks_draw_path_seg_by_seg(cairo_t *cr, dt_masks_form_gui_t *mask_gui, const int form_index,
                                   const float *points, const int points_count, const int node_count,
                                   const float zoom_scale, const gboolean round_ends)
{
  if(IS_NULL_PTR(cr) || IS_NULL_PTR(points) || IS_NULL_PTR(mask_gui)) return;
  if(node_count <= 0 || points_count <= node_count * 3 + 6) return;

  const int total_coords = points_count * 2;
  if(total_coords <= (node_count * 6 + 1)) return;

  const gboolean group_selected = (mask_gui->group_selected == form_index);

  /* The last segment there is to draw. An OPEN path -- a brush, the only caller asking for round
   * ends, since only an open path has two true ends -- has one segment fewer than it has nodes; a
   * closed one also has the segment returning to node 0. A shape still being created has no
   * closing segment either: the last one is the one the cursor is dragging. */
  const int last_segment_index = (round_ends || mask_gui->creation) ? node_count - 2 : node_count - 1;

  /* Round only the OUTWARD side of the two true ends of the WHOLE path (e.g. a brush stroke), not
   * every segment's own two ends -- that would also round every interior node joint, which is a
   * line JOIN, not a cap, and looks like a bump at each node instead of the intended smooth stroke
   * tip. cairo_line_cap is a property of the whole stroke() call, and each segment below is
   * stroked independently with CAIRO_LINE_CAP_BUTT (so that per-segment selection/dash state
   * works AND consecutive segments join flush, without a cap bump at every node), so there is no
   * single stroke covering just the two true ends to set a cap on.
   *
   * Instead: paint a full disc (radius = half the line width) at each true end FIRST, before any
   * segment is stroked. The first and last segments below then get their normal, unchanged
   * BUTT-capped stroke painted OVER these discs -- flush at the true end, extending only inward --
   * which exactly covers the inward half of each disc (same centerline, same width, same colour
   * passes) and leaves only the outward half showing: precisely what CAIRO_LINE_CAP_ROUND draws
   * for a real one-sided cap. Node positions come straight from the node header (points[i*6+2/+3]
   * is node i's own coordinate), not the tessellated tail, so this needs no walk of the outline
   * and no assumption about whether that tail wraps back to node 0. */
  if(round_ends)
  {
    const gboolean all_selected = group_selected
                                  && dt_masks_is_anything_selected(mask_gui)
                                  && (mask_gui->form_selected || mask_gui->form_dragging);

    const double start_x = points[2];
    const double start_y = points[3];
    const double end_x = points[(node_count - 1) * 6 + 2];
    const double end_y = points[(node_count - 1) * 6 + 3];

    cairo_move_to(cr, start_x, start_y);
    cairo_line_to(cr, start_x, start_y);
    dt_draw_stroke_line(DT_MASKS_NO_DASH, FALSE, cr, all_selected, zoom_scale, CAIRO_LINE_CAP_ROUND);

    cairo_move_to(cr, end_x, end_y);
    cairo_line_to(cr, end_x, end_y);
    dt_draw_stroke_line(DT_MASKS_NO_DASH, FALSE, cr, all_selected, zoom_scale, CAIRO_LINE_CAP_ROUND);
  }

  int show_segment_index = 1;
  int current_segment_index = 0;
  cairo_move_to(cr, points[node_count * 6], points[node_count * 6 + 1]);

  /* Emit at screen resolution, not at the resolution the outline was built with.
   *
   * The outline is sampled one point per RAW pixel along the path -- measured on #1158, a mean of
   * 89 150 points per shape and a maximum of 208 095, for a mean of 11.9 nodes -- while the view
   * it is drawn into is a fraction of that: at the zoom in those logs, roughly four outline points
   * land inside every device pixel. Issuing a cairo_line_to() for each of them costs 93 ms per
   * shape and draws a curve indistinguishable from the one below, which is 14.87 s of a 30.45 s
   * session spent on segments shorter than a pixel.
   *
   * Points nearer than half a device pixel to the last EMITTED one are dropped -- not skipped in
   * the walk: every point is still tested for a segment boundary below, and a boundary always
   * emits, so the strokes still start and end exactly where the nodes are. */
  const double min_step = dt_draw_min_emit_step(cr);
  const double min_step2 = min_step * min_step;
  double last_x = points[node_count * 6];
  double last_y = points[node_count * 6 + 1];

  for(int point_index = node_count * 3; point_index < points_count; point_index++)
  {
    const int coord_index = point_index * 2;
    if((coord_index + 1) >= total_coords) break;

    const double coord_x = points[coord_index];
    const double coord_y = points[coord_index + 1];

    const double step_x = coord_x - last_x;
    const double step_y = coord_y - last_y;
    const gboolean far_enough = (step_x * step_x + step_y * step_y) >= min_step2;
    if(far_enough)
    {
      cairo_line_to(cr, coord_x, coord_y);
      last_x = coord_x;
      last_y = coord_y;
    }

    const int segment_coord_index = show_segment_index * 6;
    if((segment_coord_index + 3) >= total_coords) continue;

    const double segment_x = points[segment_coord_index + 2];
    const double segment_y = points[segment_coord_index + 3];
    if(coord_x == segment_x && coord_y == segment_y)
    {
      // a segment ends exactly on a node: emit it even when it was too near to the last one,
      // or the stroke would stop short of the handle it belongs to
      if(!far_enough)
      {
        cairo_line_to(cr, coord_x, coord_y);
        last_x = coord_x;
        last_y = coord_y;
      }

      const gboolean seg_is_selected = group_selected
                                       && (dt_masks_gui_selected_segment_index(mask_gui)
                                           == current_segment_index);
      const gboolean all_selected = group_selected
                                    && dt_masks_is_anything_selected(mask_gui)
                                    && (mask_gui->form_selected || mask_gui->form_dragging);

      if(mask_gui->creation && current_segment_index == node_count - 2)
        dt_draw_stroke_line(DT_MASKS_DASH_ROUND, FALSE, cr, all_selected, zoom_scale, CAIRO_LINE_CAP_ROUND);
      else
        dt_draw_stroke_line(DT_MASKS_NO_DASH, FALSE, cr, (seg_is_selected || all_selected), zoom_scale,
                            CAIRO_LINE_CAP_BUTT);

      show_segment_index = (show_segment_index + 1) % node_count;
      current_segment_index++;

      /* Every segment has been stroked. What the array still holds past this node is not part of
       * the outline: for a brush it is the same centerline recorded a second time in the opposite
       * direction (the border wraps around the stroke, so the line under it is walked there and
       * back -- see _brush_get_pts_border()), and for a shape being created it is the segment that
       * will close it once it exists. Walking further only re-detects nodes in reverse order and
       * strokes them again. */
      if(current_segment_index > last_segment_index) break;

      // dt_draw_stroke_line() consumed the path; the next one starts here
      cairo_move_to(cr, coord_x, coord_y);
    }
  }

  /* Leave nothing behind. The walk above stops on a node boundary, so the points that follow it
   * are still an un-stroked path in `cr', and cairo keeps a path across calls: the next stroke
   * ANYWHERE picks it up and paints it in ITS own style -- this shape's own dashed border draws
   * the leftover dashed, another shape's hover highlight draws it highlighted, and with nothing
   * drawn after it at all it never appears. */
  cairo_new_path(cr);
}

/**
 * @brief Draw completed shapes from the current creation session.
 *
 * During continuous creation, the GUI-visible form must remain the unfinished
 * shape because creation previews and mouse handlers read it directly. The ids
 * stored in creation_formids are therefore drawn explicitly here, so only the
 * shapes created in this session stay visible until creation mode is exited.
 */
/* The creation session's own outline cache.
 *
 * GUI-thread state, like every other dt_masks_form_gui_t. It is static because the shapes it
 * describes outlive a single expose and nothing else owns them: `creation_formids' is a list of
 * ids, and the outlines built from them used to live in a STACK LOCAL that was initialised fresh
 * every expose, with its cache key forced to zero after each shape. That defeated the cache by
 * construction -- every shape drawn since creation mode was entered was re-derived and thrown away
 * on every frame.
 *
 * Measured on #1158, with the list length as the variable: 0 shapes 0.0000 s, 1 shape 0.1141 s,
 * 2 shapes 0.2431 s, 3 shapes 0.3189 s, 4 shapes 0.4444 s -- about 110 ms per shape per expose,
 * while the mask GROUP, whose outlines ARE cached, drew all of its shapes in 0.0020 s. Everything
 * else in that stage measured zero: the predicates, the preamble, the group push, the outline
 * refresh, the composite, the hit test.
 */
static dt_masks_form_gui_t _session_gui = { 0 };
static gboolean _session_gui_inited = FALSE;
static uint64_t _session_gui_generation = 0;
static int _session_gui_count = 0;

/* The shapes already saved in this session, rendered.
 *
 * They do not change while a new stroke is being drawn -- that is what makes them "already saved"
 * -- so re-stroking them on every frame is redrawing a still image. Measured: 8.8 ms per shape per
 * frame with the outline cached and decimated, 15.9 ms of a 25.7 ms redraw, and it is rasterisation
 * rather than geometry (only the path is stroked for these; borders, nodes and handles all sit
 * behind `group_selected == index', which is never true here).
 *
 * A cairo pattern from cairo_pop_group() is that rendering, kept. Painting it costs one composite
 * whatever the shape count, and cairo owns the surface behind it so there is nothing to size or
 * free by hand.
 *
 * The key has to include the MATRIX: a group's pattern carries the transform in effect when it was
 * pushed, so painting it under a different one would put the shapes somewhere else. Panning or
 * zooming therefore re-renders, which is correct and is not a per-frame cost while drawing. */
static cairo_pattern_t *_session_pattern = NULL;
static uint64_t _session_pattern_key = 0;

/* The area those shapes actually occupy, in the same coordinates they are drawn in.
 *
 * Both halves of this cost the whole window otherwise: the group is sized to the CLIP at push
 * time, and painting its pattern fills the clip. Measured with no clip: a cache HIT still costs
 * 7.5 ms -- compositing a device-scaled full-frame ARGB surface -- and a miss 19.3 ms, together
 * 3.13 s of a 5.77 s redraw total. Shapes that cover a fifth of the frame should cost a fifth of
 * that, and clipping to their union is what says so to cairo. */
static gboolean _session_bbox_valid = FALSE;
static double _session_bbox[4] = { 0.0, 0.0, 0.0, 0.0 };   /**< x0, y0, x1, y1 */

/* Did the outlines actually move?
 *
 * The rendering is keyed on the geometry GENERATION, which is deliberately a version and not a
 * content hash -- over-invalidating is the safe direction for the geometry service, whose consumers
 * mostly recompute something cheap. Here it is not cheap: committing a stroke advances the
 * generation, and re-rendering costs ~18 ms per shape in the session, 218 ms at twelve of them.
 * That is the whole of the tail.
 *
 * But a generation bump does not mean the outlines moved. This asks whether they did, from the
 * outlines themselves: the point count and the two endpoints of every shape. Anything that moves an
 * outline -- a crop, a rotation, a scale, a keystone -- shifts every coordinate in it, endpoints
 * included, and a shape edited into a different path changes its point count or its ends. What it
 * cannot see is a change that preserves both endpoints AND the exact point count while moving
 * points in between; no geometry module does that, and an edit that did would have committed a
 * different point count on the way.
 *
 * Cheap on purpose: three numbers per shape, against hashing 89 000 points each. */
typedef struct _session_outline_sig_t
{
  int points_count;
  float first[2];
  float last[2];
} _session_outline_sig_t;

static _session_outline_sig_t *_session_sigs = NULL;
static int _session_sigs_count = 0;

static void _session_sigs_reset(void)
{
  dt_free_align(_session_sigs);
  _session_sigs_count = 0;
}

/** @brief Take the signature of every cached outline. @return the number taken. */
static int _session_sigs_take(const dt_masks_form_gui_t *gui, _session_outline_sig_t *out, const int max)
{
  int taken = 0;
  for(const GList *node = gui->points; node && taken < max; node = g_list_next(node))
  {
    const dt_masks_form_gui_points_t *const pts = (const dt_masks_form_gui_points_t *)node->data;
    if(IS_NULL_PTR(pts)) continue;

    out[taken].points_count = pts->points_count;
    if(!IS_NULL_PTR(pts->points) && pts->points_count > 0)
    {
      out[taken].first[0] = pts->points[0];
      out[taken].first[1] = pts->points[1];
      out[taken].last[0] = pts->points[2 * (pts->points_count - 1)];
      out[taken].last[1] = pts->points[2 * (pts->points_count - 1) + 1];
    }
    else
      out[taken].first[0] = out[taken].first[1] = out[taken].last[0] = out[taken].last[1] = 0.0f;
    taken++;
  }
  return taken;
}

/** @brief Union of the cached outlines, or FALSE when there is nothing to bound. */
static gboolean _session_bbox_from_points(const dt_masks_form_gui_t *gui, double bbox[4])
{
  gboolean any = FALSE;
  for(const GList *node = gui->points; node; node = g_list_next(node))
  {
    const dt_masks_form_gui_points_t *const pts = (const dt_masks_form_gui_points_t *)node->data;
    if(IS_NULL_PTR(pts) || IS_NULL_PTR(pts->points) || pts->points_count <= 0) continue;

    for(int i = 0; i < pts->points_count; i++)
    {
      const double x = pts->points[2 * i];
      const double y = pts->points[2 * i + 1];

      if(!any)
      {
        bbox[0] = bbox[2] = x;
        bbox[1] = bbox[3] = y;
        any = TRUE;
      }
      else
      {
        bbox[0] = fmin(bbox[0], x);
        bbox[1] = fmin(bbox[1], y);
        bbox[2] = fmax(bbox[2], x);
        bbox[3] = fmax(bbox[3], y);
      }
    }
  }
  return any;
}

/** @brief Clip to the session's area, with room for the stroke's own width. */
static gboolean _session_clip(cairo_t *cr, const float zoom_scale)
{
  if(!_session_bbox_valid) return FALSE;

  const double margin = (zoom_scale > 1e-6f) ? (16.0 / zoom_scale) : 0.0;
  cairo_save(cr);
  cairo_rectangle(cr, _session_bbox[0] - margin, _session_bbox[1] - margin,
                  (_session_bbox[2] - _session_bbox[0]) + 2.0 * margin,
                  (_session_bbox[3] - _session_bbox[1]) + 2.0 * margin);
  cairo_clip(cr);
  cairo_new_path(cr);
  return TRUE;
}

/** @brief Drop the session outlines. Safe to call when nothing is cached. */
static void _session_pattern_reset(void)
{
  if(!IS_NULL_PTR(_session_pattern)) cairo_pattern_destroy(_session_pattern);
  _session_pattern = NULL;
  _session_pattern_key = 0;
}

static void _session_bbox_invalidate(void)
{
  _session_bbox_valid = FALSE;
}

/** @brief What the cached rendering is a rendering OF. */
static uint64_t _session_pattern_hash(cairo_t *cr, const dt_masks_form_gui_t *creation_gui,
                                      const uint64_t generation, const int count)
{
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);

  /* No generation here: whether the geometry MOVED is asked of the outlines themselves
   * (_session_sigs_take), because the generation advances on every history commit -- including the
   * one that saves the stroke being drawn -- without shifting any of these shapes. What remains in
   * the key is what genuinely describes this rendering: where it was drawn, and which shapes. */
  (void)generation;
  uint64_t hash = 5381;
  hash = dt_hash(hash, (const char *)&matrix, sizeof(matrix));
  hash = dt_hash(hash, (const char *)&count, sizeof(count));
  for(const GList *node = creation_gui->creation_formids; node; node = g_list_next(node))
  {
    const int formid = GPOINTER_TO_INT(node->data);
    hash = dt_hash(hash, (const char *)&formid, sizeof(formid));
  }
  return hash ? hash : 1;
}

static void _session_gui_reset(void)
{
  _session_pattern_reset();
  _session_bbox_invalidate();
  _session_sigs_reset();
  if(!_session_gui_inited) return;
  g_list_free_full(_session_gui.points, dt_masks_form_gui_points_free);
  _session_gui.points = NULL;
  _session_gui_generation = 0;
  _session_gui_count = 0;
}

static void _masks_draw_creation_session_forms(dt_develop_t *develop, dt_iop_module_t *module,
                                               cairo_t *cr, const float zoom_scale,
                                               const dt_masks_form_gui_t *creation_gui)
{
  /* Nothing to draw and nothing to forget.
   *
   * `creation' goes false between strokes -- a shape is committed, then the next one begins -- and
   * resetting on that threw the session's rendering away every time: 46 of 78 cache misses in one
   * log had no reason to report beyond the pattern having been destroyed, at ~21 ms each. The
   * shapes it describes are still there, and will be drawn again the moment creation resumes.
   *
   * What ends the session is the LIST emptying, which is the only thing reset on here now. */
  if(IS_NULL_PTR(creation_gui->creation_formids))
  {
    _session_gui_reset();
    return;
  }

  const int count = g_list_length(creation_gui->creation_formids);

  if(!creation_gui->creation)
  {
    // Nothing to draw between strokes. Said out loud so a log reader does not count it as a cache
    // miss: the outer timer around this call fires either way.
    if(dt_get_debug_flags() & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks] creation session idle (%d shapes kept)\n", count);
    return;
  }

  const uint64_t live_generation = dt_geometry_chain_generation(develop->geometry_chain);

  if(!_session_gui_inited)
  {
    dt_masks_init_form_gui(develop, &_session_gui);
    _session_gui_inited = TRUE;
    _session_gui_generation = 0;
    _session_gui_count = 0;
  }
  _session_gui.dev = develop;
  _session_gui.edit_mode = creation_gui->edit_mode;
  _session_gui.group_selected = -1;

  /* Rebuild only when the composed geometry moved or the session gained a shape -- the same rule
   * the mask group's outlines follow. A shape's own content changing commits history, which
   * rebuilds the chain, which advances the generation. */
  const gboolean rebuild = (_session_gui_generation != live_generation) || (_session_gui_count != count);
  if(rebuild)
  {
    g_list_free_full(_session_gui.points, dt_masks_form_gui_points_free);
    _session_gui.points = NULL;
  }

  /* The outlines are current from here on. Ask whether they actually MOVED before deciding the
   * rendering is stale: the generation advances when a stroke is committed, which does not shift a
   * single point of the shapes already saved. */
  gboolean outlines_moved = TRUE;
  if(rebuild)
  {
    _session_outline_sig_t *const sigs
        = (_session_outline_sig_t *)dt_alloc_align(sizeof(_session_outline_sig_t) * MAX(count, 1));
    const int taken = IS_NULL_PTR(sigs) ? 0 : _session_sigs_take(&_session_gui, sigs, count);

    outlines_moved = IS_NULL_PTR(_session_sigs) || taken != _session_sigs_count
                     || IS_NULL_PTR(sigs)
                     || memcmp(sigs, _session_sigs, sizeof(_session_outline_sig_t) * taken) != 0;

    if(!IS_NULL_PTR(sigs))
    {
      dt_free_align(_session_sigs);
      _session_sigs = sigs;
      _session_sigs_count = taken;
    }
  }
  else
    outlines_moved = FALSE;

  /* Nothing about these shapes changed: paint the rendering we already have. */
  const uint64_t pattern_key = _session_pattern_hash(cr, creation_gui, live_generation, count);

  if((dt_get_debug_flags() & DT_DEBUG_PERF) && (outlines_moved || pattern_key != _session_pattern_key))
  {
    cairo_matrix_t matrix;
    cairo_get_matrix(cr, &matrix);
    dt_print(DT_DEBUG_MASKS,
             "[masks] session re-render: rebuild=%d moved=%d generation %lu->%lu count %d->%d"
             " matrix (%.4f %.4f %.4f %.4f %.2f %.2f)\n",
             rebuild, outlines_moved, (unsigned long)_session_gui_generation,
             (unsigned long)live_generation, _session_gui_count, count, matrix.xx, matrix.yx,
             matrix.xy, matrix.yy, matrix.x0, matrix.y0);
  }
  if(!outlines_moved && !IS_NULL_PTR(_session_pattern) && pattern_key == _session_pattern_key)
  {
    const gboolean clipped = _session_clip(cr, zoom_scale);
    cairo_set_source(cr, _session_pattern);
    cairo_paint(cr);
    if(clipped) cairo_restore(cr);
    if(dt_get_debug_flags() & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks] creation session (%d shapes) painted from cache\n", count);
    return;
  }

  _session_pattern_reset();

  /* Bound the group to what the previous render occupied -- but ONLY when the shapes being drawn
   * into it are the same ones that bbox was measured from (rebuild == FALSE: nothing but the
   * matrix moved). On a rebuild the bbox is stale by construction -- it describes the PREVIOUS
   * shape set, recomputed from the fresh one only AFTER this group is painted -- so clipping to it
   * here would cut off exactly the part of a newly added or changed shape that falls outside what
   * the old shapes occupied. That clipped result is what gets cached as `_session_pattern`, so the
   * shape stays silently truncated on every following "painted from cache" replay, not just this
   * one frame. A brush/polygon stroke reaching outside the old bbox is the visible case: part or
   * all of its outline never gets recorded into the group in the first place. */
  const gboolean clipped = !rebuild && _session_clip(cr, zoom_scale);
  cairo_push_group(cr);

  // Iterate over the ids saved in this creation session. Other masks stay
  // hidden while creation is active, even if they belong to the same module.
  int index = 0;
  for(GList *formid_node = creation_gui->creation_formids; formid_node; formid_node = g_list_next(formid_node))
  {
    const int formid = GPOINTER_TO_INT(formid_node->data);
    dt_masks_form_t *session_form = dt_masks_get_from_id(develop, formid);
    if(IS_NULL_PTR(session_form)) continue;

    /* Built at `index', not at 0: one slot per shape, so they can all be kept. The old loop wrote
     * every shape into slot 0 and freed it again, which is why none of them could be cached. */
    const double shape_start = dt_get_wtime();
    if(rebuild) dt_masks_gui_form_create(session_form, &_session_gui, index, module);
    const double built = dt_get_wtime();

    if(session_form->functions && session_form->functions->post_expose)
    {
      const guint point_count = g_list_length(session_form->points);
      _session_gui.type = session_form->type;
      session_form->functions->post_expose(cr, zoom_scale, &_session_gui, index, point_count);
    }

    /* Which half costs: rebuilding an outline, or stroking it. The session is 15.9 ms of a 25.7 ms
     * redraw, about 5 ms per shape, while the shape being drawn -- same stroker, cached outline --
     * costs 1.4 ms. Either the cache is not holding here or the draw is genuinely dearer, and
     * those want opposite fixes. */
    if(dt_get_debug_flags() & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks] session shape %d: %s %0.04f sec, drawn %0.04f sec\n", index,
               rebuild ? "rebuilt in" : "cached,", built - shape_start, dt_get_wtime() - built);

    // Keep the saved-session shape path from being connected to the active
    // creation cursor drawn right after this loop.
    cairo_new_path(cr);
    index++;
  }

  /* Keep the rendering, and paint it -- this frame included, so the first frame after a change
   * costs the same as it did before and every later one costs a composite. */
  _session_pattern = cairo_pop_group(cr);
  _session_pattern_key = pattern_key;
  cairo_set_source(cr, _session_pattern);
  cairo_paint(cr);
  if(clipped) cairo_restore(cr);

  /* The bbox is a function of the OUTLINES, so it is recomputed when they are and not when the
   * matrix moved under them -- walking every point of every shape is not free (measured: doing it
   * on every render took the miss path from 19.3 ms to 25.1 ms, more than the clip saved). */
  if(rebuild) _session_bbox_valid = _session_bbox_from_points(&_session_gui, _session_bbox);

  if(rebuild)
  {
    _session_gui_generation = live_generation;
    _session_gui_count = count;
  }
}

void dt_masks_events_post_expose(dt_develop_t *dev, struct dt_iop_module_t *module, cairo_t *cr,
                                 int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_masks_events_post_expose_with(dev, module, cr, width, height, pointerx, pointery, NULL);
}

void dt_masks_events_post_expose_with(dt_develop_t *dev, struct dt_iop_module_t *module, cairo_t *cr,
                                      int32_t width, int32_t height, int32_t pointerx, int32_t pointery,
                                      const dt_masks_overlay_transform_t *transform)
{
  const double post_expose_start = dt_get_wtime();
  dt_develop_t *develop = dev;
  if(IS_NULL_PTR(develop)) return;
  dt_masks_form_t *mask_form = dt_masks_get_visible_form(develop);
  dt_masks_form_gui_t *mask_gui = develop->form_gui;
  if(IS_NULL_PTR(mask_gui)) return;
  if(IS_NULL_PTR(mask_form)) return;

  int buffer_width = 0;
  int buffer_height = 0;
  dt_dev_get_processed_size(develop, &buffer_width, &buffer_height);

  if(buffer_width < 1.0 || buffer_height < 1.0) return;
  const float zoom_scale = IS_NULL_PTR(transform) ? dt_dev_get_zoom_level(develop) : (float)transform->scale;

  /* Draw into an isolated group, so that overlapping shapes composite once against the view
   * instead of once each.
   *
   * This used to allocate a full-window ARGB surface with cairo_surface_create_similar() on EVERY
   * expose and destroy it again at the end -- at ppd 2 that is a four-byte-per-pixel buffer of
   * four times the window's area, created, cleared, composited and freed per frame, and on an
   * X11 or GL target it is a server-side pixmap each time. Measured on #1158: the expose stage
   * that contains it costs 0.44 s while the mask drawing inside it costs 0.0014 s, and it grows
   * across a session -- 11 ms, 137, 221, 310 -- which is what repeated large allocation and
   * release looks like.
   *
   * cairo_push_group() is the same isolation with none of that: cairo sizes the group to the
   * current CLIP rather than the window, and takes it from its own scratch pool instead of
   * allocating. Every path out of here must pop what it pushed. */
  const double group_start = dt_get_wtime();
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] overlay preamble took %0.04f sec\n", group_start - post_expose_start);
  cairo_push_group(cr);
  cairo_t *const mask_draw = cr;
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] overlay group pushed in %0.04f sec\n", dt_get_wtime() - group_start);

  // Apply the same transformation to the mask drawing context
  /*cairo_matrix_t m;
  cairo_get_matrix(cr, &m);
  cairo_set_matrix(mask_draw, &m);*/
  
  cairo_save(mask_draw);

  // We rescale to input space -- from the viewport, or from the caller's own mapping when it
  // supplied one (see dt_masks_overlay_transform_t: the viewport path needs GUI state).
  if(IS_NULL_PTR(transform))
  {
    if(dt_dev_rescale_roi_to_input(develop, mask_draw, width, height))
    {
      cairo_restore(mask_draw);
      cairo_pattern_destroy(cairo_pop_group(cr));   // discard: nothing was drawn
      return;
    }
  }
  else
  {
    cairo_translate(mask_draw, transform->offset_x, transform->offset_y);
    cairo_scale(mask_draw, transform->scale, transform->scale);
  }

  // We update the form if needed
  // Add preview when creating a circle, ellipse and gradient
  const dt_times_t rebuild_start = { 0 };
  dt_get_times((dt_times_t *)&rebuild_start);

  if(!((mask_form->type & DT_MASKS_IS_PRIMITIVE_SHAPE) && mask_gui->creation))
    dt_masks_gui_form_test_create(mask_form, mask_gui, module);

  if(dt_get_debug_flags() & DT_DEBUG_MASKS)
    dt_show_times(&rebuild_start, "[masks] overlay outline refresh");

  const double session_start = dt_get_wtime();
  _masks_draw_creation_session_forms(develop, module, mask_draw, zoom_scale, mask_gui);
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] creation session (%d shapes) drawn in %0.04f sec\n",
             g_list_length(mask_gui->creation_formids), dt_get_wtime() - session_start);

  // Draw form
  const dt_times_t draw_start = { 0 };
  dt_get_times((dt_times_t *)&draw_start);

  if(mask_form->type & DT_MASKS_GROUP)
    dt_group_events_post_expose(mask_draw, zoom_scale, mask_form, mask_gui);
  else if(mask_form->functions && mask_form->functions->post_expose)
  {
    const guint point_count = g_list_length(mask_form->points);
    mask_gui->type = mask_form->type;
    mask_form->functions->post_expose(mask_draw, zoom_scale, mask_gui, 0, point_count);
  }
  cairo_restore(mask_draw);

  if(dt_get_debug_flags() & DT_DEBUG_MASKS)
    dt_show_times(&draw_start, "[masks] overlay drawn");

  /* Composite the group. pop_group_to_source() hands back a pattern already carrying the matrix
   * that was in effect at push time, so a plain paint reproduces exactly what the explicit
   * full-window surface did -- the save/restore pair above balances the one taken after the
   * push. */
  const double composite_start = dt_get_wtime();
  cairo_pop_group_to_source(cr);
  cairo_paint(cr);
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] overlay composited in %0.04f sec\n", dt_get_wtime() - composite_start);
}

void dt_masks_clear_form_gui(dt_develop_t *develop)
{
  /* Deliberately NOT resetting the creation session's cache here.
   *
   * dt_masks_change_form_gui() calls this every time the visible form changes, which during a
   * creation session is once per stroke -- and the shapes already saved in that session are
   * unaffected by which form is currently being edited. Tying the two together threw away their
   * rendering AND their outlines on every stroke: 37 of 77 cache misses in one log came from the
   * pattern having been destroyed rather than from anything about the shapes changing, each
   * costing a 20 ms re-render.
   *
   * What ends the session cache is the session ending, which
   * _masks_draw_creation_session_forms() handles at its top, and any change to what it describes,
   * which its key covers: the geometry generation, the shape count, the shape ids and the
   * matrix. */
  if(IS_NULL_PTR(develop->form_gui)) return;
  g_list_free_full(develop->form_gui->points, dt_masks_form_gui_points_free);
  develop->form_gui->points = NULL;
  dt_masks_dynbuf_free(develop->form_gui->guipoints);
  develop->form_gui->guipoints = NULL;
  dt_masks_dynbuf_free(develop->form_gui->guipoints_payload);
  develop->form_gui->guipoints_payload = NULL;
  develop->form_gui->guipoints_count = 0;
  develop->form_gui->geometry_generation = 0;
  develop->form_gui->formid = 0;
  develop->form_gui->delta[0] = develop->form_gui->delta[1] = 0.0f;
  develop->form_gui->scrollx = develop->form_gui->scrolly = 0.0f;
  develop->form_gui->form_selected = develop->form_gui->border_selected = develop->form_gui->form_dragging
      = develop->form_gui->form_rotating = develop->form_gui->border_toggling = develop->form_gui->gradient_toggling = FALSE;
  develop->form_gui->source_selected = develop->form_gui->source_dragging = FALSE;
  develop->form_gui->pivot_selected = FALSE;
  develop->form_gui->node_hovered = -1;
  develop->form_gui->handle_hovered = -1;
  develop->form_gui->seg_hovered = -1;
  develop->form_gui->handle_border_hovered = -1;
  develop->form_gui->node_selected = FALSE;
  develop->form_gui->handle_selected = FALSE;
  develop->form_gui->seg_selected = FALSE;
  develop->form_gui->handle_border_selected = FALSE;
  develop->form_gui->node_selected_idx = -1;
  develop->form_gui->handle_border_dragging = develop->form_gui->seg_dragging = develop->form_gui->handle_dragging
      = develop->form_gui->node_dragging = -1;
  develop->form_gui->creation_closing_form = FALSE;
  dt_masks_creation_mode_quit(develop->form_gui);
  develop->form_gui->pressure_sensitivity = DT_MASKS_PRESSURE_OFF;
  develop->form_gui->creation_module = NULL;
  develop->form_gui->creation_type = DT_MASKS_NONE;
  g_list_free(develop->form_gui->creation_formids);
  develop->form_gui->creation_formids = NULL;
  develop->form_gui->creation_last_formid = 0;
  develop->form_gui->node_selected = FALSE;

  develop->form_gui->group_selected = -1;
  develop->form_gui->group_selected = -1;
  develop->form_gui->edit_mode = DT_MASKS_EDIT_OFF;
  develop->form_gui->last_rebuild_ts = 0.0;
  develop->form_gui->last_rebuild_pos[0] = develop->form_gui->last_rebuild_pos[1] = 0.0f;
  develop->form_gui->rebuild_pending = FALSE;
  develop->form_gui->last_hit_test_pos[0] = develop->form_gui->last_hit_test_pos[1] = -1.0f;
  // allow to select a shape inside an iop
  dt_masks_select_form(develop, NULL, NULL);
}

void dt_masks_change_form_gui(dt_develop_t *dev, dt_masks_form_t *new_form)
{
  dt_masks_form_t *old_form = dt_masks_get_visible_form(dev);
  if(!IS_NULL_PTR(old_form))
  {
    gboolean is_registered = FALSE;
    gboolean is_registered_for_cleanup = FALSE;
    dt_pthread_rwlock_rdlock(&dev->masks_mutex);
    for(const GList *form_node = dev->forms; form_node; form_node = g_list_next(form_node))
    {
      if(form_node->data == old_form)
      {
        is_registered = TRUE;
        break;
      }
    }

    for(const GList *form_node = dev->allforms; form_node; form_node = g_list_next(form_node))
    {
      if(form_node->data == old_form)
      {
        is_registered_for_cleanup = TRUE;
        break;
      }
    }
    dt_pthread_rwlock_unlock(&dev->masks_mutex);

    // Free only fully orphan temporary previews. Forms tracked in either list
    // are owned by develop and will be released by its teardown path.
    //
    // form_visible is cleared BEFORE the release, not after. old_form IS what it points at
    // -- it came from dt_masks_get_visible_form(dev) at the top -- so releasing it first
    // leaves the field holding freed memory until dt_masks_set_visible_form() below, and
    // dt_masks_clear_form_gui() runs inside that window. Re-entering this function there
    // reads the dangling form_visible as its own old_form, finds it in neither list because
    // it is already gone, and releases it a second time (Sentry 140265757: SIGSEGV in
    // dt_masks_change_form_gui, two frames of it, from the view-leave teardown path).
    // The creation-exit path in this file already orders it this way.
    //
    // Released through unref rather than dt_masks_free_form(): forms are refcounted and
    // free_form is the destructor unref calls at zero. Calling it directly frees the form
    // even when a history snapshot still references it. An orphan preview holds the single
    // reference it was created with, so this frees it exactly as before.
    if(!is_registered && !is_registered_for_cleanup)
    {
      dt_masks_set_visible_form(dev, NULL);
      dt_masks_form_unref(old_form);
    }
  }

  dt_masks_clear_form_gui(dev);
  dt_masks_set_visible_form(dev, new_form);
}

void dt_masks_reset_form_gui(dt_develop_t *dev)
{
  dt_masks_change_form_gui(dev, NULL);
  dt_masks_shape_buttons_deactivate_all(NULL);
  dt_iop_module_t *module = dev->gui_module;
  if(!IS_NULL_PTR(module) && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(module->flags() & IOP_FLAGS_NO_MASKS)
    && !IS_NULL_PTR(module->gui) && !IS_NULL_PTR(module->gui->blend_data))
  {
    dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->gui->blend_data;
    blend_data->masks_shown = DT_MASKS_EDIT_OFF;
    if(!IS_NULL_PTR(blend_data->masks_edit))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_edit), 0);
  }
}

void dt_masks_reset_show_masks_icons(dt_develop_t *dev)
{
  dt_masks_shape_buttons_deactivate_all(NULL);
  for(GList *module_node = dev->iop; module_node; module_node = g_list_next(module_node))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)module_node->data;
    if(module && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(module->flags() & IOP_FLAGS_NO_MASKS)
    && !IS_NULL_PTR(module->gui) && !IS_NULL_PTR(module->gui->blend_data))
    {
      dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->gui->blend_data;
      blend_data->masks_shown = DT_MASKS_EDIT_OFF;
      if(!IS_NULL_PTR(blend_data->masks_edit))
      {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_edit), FALSE);
        gtk_widget_queue_draw(blend_data->masks_edit);
      }
    }
  }
}

static void _menu_no_masks(struct dt_iop_module_t *module)
{
  // we drop all the forms in the iop
  dt_masks_form_t *group_form = _group_from_module(module->dev, module);
  if(group_form) dt_masks_form_delete(module->dev, module, NULL, group_form);
  module->blend_params->mask_id = 0;

  // and we update the iop
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  dt_iop_gui_blend_masks_update(module);
}

static void _menu_add_shape(struct dt_iop_module_t *module, dt_masks_type_t type)
{
  dt_masks_creation_mode_enter(module->dev, module, type);
}

static void _menu_add_exist(dt_iop_module_t *module, int form_id)
{
  if(IS_NULL_PTR(module)) return;
  dt_masks_form_t *mask_form = dt_masks_get_from_id(module->dev, form_id);
  if(IS_NULL_PTR(mask_form)) return;

  // is there already a masks group for this module ?
  dt_masks_form_t *group_form = _group_from_module(module->dev, module);
  if(IS_NULL_PTR(group_form))
  {
    group_form = _group_create(module->dev, module, DT_MASKS_GROUP);
  }
  group_form = dt_masks_cow_touch(module->dev, group_form);
  // we add the form in this group
  dt_masks_group_add_form(module->dev, group_form, mask_form);
  // we save the group
  // and we ensure that we are in edit mode

  dt_iop_gui_blend_masks_update(module);
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
}

void dt_masks_group_update_name(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;
  dt_masks_form_t *group_form = _group_from_module(module->dev, module);
  if (IS_NULL_PTR(group_form))
    return;

  _set_group_name_from_module(module, group_form);

  dt_iop_gui_blend_masks_update(module);
}

void dt_masks_iop_combo_populate(GtkWidget *widget, void *data)
{
  // we ensure that the module has focus
  dt_iop_module_t *module = (dt_iop_module_t *)data;
  dt_iop_request_focus(module);
  dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->gui->blend_data;

  // we determine a higher approx of the entry number
  const guint forms_count = g_list_length(module->dev->forms);
  const guint iop_count = g_list_length(module->dev->iop);
  guint combo_capacity = 5 + forms_count + iop_count;
  dt_free_align(blend_data->masks_combo_ids);
  blend_data->masks_combo_ids = dt_alloc_align(sizeof(int) * combo_capacity);

  int *combo_ids = blend_data->masks_combo_ids;
  GtkWidget *combo = blend_data->masks_combo;

  // we remove all the combo entries except the first one
  while(dt_bauhaus_combobox_length(combo) > 1)
  {
    dt_bauhaus_combobox_remove_at(combo, 1);
  }

  int combo_index = 0;
  combo_ids[combo_index] = 0; // nothing to do for the first entry (already here)
  combo_index++;

  // add existing shapes
  dt_pthread_rwlock_rdlock(&module->dev->masks_mutex);
  for(GList *form_node = module->dev->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)form_node->data;
    if((mask_form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
       || mask_form->formid == module->blend_params->mask_id)
    {
      continue;
    }

    // we search were this form is used in the current module
    int is_used = 0;
    dt_masks_form_t *group_form = _group_from_module(module->dev, module);
    if(group_form && (group_form->type & DT_MASKS_GROUP))
    {
      for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
      {
        dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
        if(group_entry->formid == mask_form->formid)
        {
          is_used = 1;
          break;
        }
      }
    }
    if(!is_used)
    {
      dt_bauhaus_combobox_add(combo, mask_form->name);
      combo_ids[combo_index] = mask_form->formid;
      combo_index++;
    }
  }
  dt_pthread_rwlock_unlock(&module->dev->masks_mutex);

  // masks from other iops
  int iop_index = 1;
  for(GList *module_node = module->dev->iop; module_node; module_node = g_list_next(module_node))
  {
    dt_iop_module_t *other_module = (dt_iop_module_t *)module_node->data;
    if((other_module != module) && (other_module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
       && !(other_module->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_masks_form_t *group_form = _group_from_module(module->dev, other_module);
      if(group_form)
      {
        gchar *module_label = dt_history_item_get_name(other_module);
        dt_bauhaus_combobox_add(combo, g_strdup_printf(_("reuse shapes from %s"), module_label));
        dt_free(module_label);
        combo_ids[combo_index] = -1 * iop_index;
        combo_index++;
      }
    }
    iop_index++;
  }
}

void dt_masks_iop_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module)
{
  // we get the corresponding value
  dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->gui->blend_data;

  int selection_index = dt_bauhaus_combobox_get(blend_data->masks_combo);
  if(selection_index == 0) return;
  if(selection_index > 0)
  {
    int selection_value = blend_data->masks_combo_ids[selection_index];
    const guint iop_count = g_list_length(module->dev->iop);
    // FIXME : these values should use binary enums
    if(selection_value == -1000000)
    {
      // delete all masks
      _menu_no_masks(module);
    }
    else if(selection_value == -2000001)
    {
      // add a circle shape
      _menu_add_shape(module, DT_MASKS_CIRCLE);
    }
    else if(selection_value == -2000002)
    {
      // add a path shape
      _menu_add_shape(module, DT_MASKS_POLYGON);
    }
    else if(selection_value == -2000016)
    {
      // add a gradient shape
      _menu_add_shape(module, DT_MASKS_GRADIENT);
    }
    else if(selection_value == -2000032)
    {
      // add a gradient shape
      _menu_add_shape(module, DT_MASKS_ELLIPSE);
    }
    else if(selection_value == -2000064)
    {
      // add a brush shape
      _menu_add_shape(module, DT_MASKS_BRUSH);
    }
    else if(selection_value < 0)
    {
      // use same shapes as another iop
      selection_value = -1 * selection_value - 1;
      if(selection_value < (int)iop_count)
      {
        dt_iop_module_t *source_module
            = (dt_iop_module_t *)g_list_nth_data(module->dev->iop, selection_value);
        dt_masks_iop_use_same_as(module, source_module);
        // and we ensure that we are in edit mode
        //

        dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      }
    }
    else if(selection_value > 0)
    {
      // add an existing shape
      _menu_add_exist(module, selection_value);
    }
    else
      return;
  }
  // we update the combo line
  dt_iop_gui_blend_masks_update(module);
  dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
}

float dt_masks_form_get_interaction_value(dt_develop_t *dev, const int group_id, const int formid,
                                          dt_masks_interaction_t interaction)
{
  if(IS_NULL_PTR(dev)) return NAN;

  if(interaction == DT_MASKS_INTERACTION_OPACITY)
  {
    /* Opacity is a property of the MEMBERSHIP, not of the shape: the same shape referenced by two
     * groups carries two opacities, which is why this one needs the group id and the others do
     * not. */
    dt_masks_member_t member = { 0 };
    if(dt_masks_group_get_member(dev, group_id, formid, &member) != DT_MASKS_OK) return NAN;
    return member.opacity;
  }

  dt_masks_form_t *target_form = dt_masks_get_from_id(dev, formid);
  if(IS_NULL_PTR(target_form) || IS_NULL_PTR(target_form->functions) || IS_NULL_PTR(target_form->functions->get_interaction_value)) return NAN;

  return target_form->functions->get_interaction_value(target_form, interaction);
}

gboolean dt_masks_form_get_gravity_center(dt_develop_t *dev, const dt_masks_form_t *mask_form, float center[2], float *area)
{
  center[0] = 0.0f;
  center[1] = 0.0f;
  if(!IS_NULL_PTR(area)) *area = 0.0f;

  if(IS_NULL_PTR(mask_form) || IS_NULL_PTR(mask_form->functions) || IS_NULL_PTR(mask_form->functions->get_gravity_center) || IS_NULL_PTR(center)) return FALSE;
  return mask_form->functions->get_gravity_center(dev, mask_form, center, area);
}

/**
 * @brief Center the darkroom ROI on a mask form gravity center.
 *
 * @details Mask forms store their gravity center in normalized RAW coordinates,
 * while `dt_dev_viewport_center_x(dev)` and `dt_dev_viewport_center_y(dev)` address the processed image. Transforming
 * through absolute RAW and processed-image coordinates keeps the center aligned
 * with the final image after distortion modules, then the ROI clamp preserves
 * the same bounds used by manual panning.
 *
 * @return 0 on success, 1 when the form has no usable center or the transform fails.
 */
int dt_masks_center_view_on_form(dt_develop_t *dev, const dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(mask_form)) return 1;

  float center[2] = { 0.0f, 0.0f };
  float area = 0.0f;
  if(!dt_masks_form_get_gravity_center(dev, mask_form, center, &area)) return 1;

  dt_dev_coordinates_raw_norm_to_raw_abs(dev, center, 1);
  if(!dt_dev_coordinates_raw_abs_to_image_abs(dev, center, 1)) return 1;
  dt_dev_coordinates_image_abs_to_image_norm(dev, center, 1);

  dt_dev_viewport_set_center(dev, center[0], center[1]);
  dt_dev_clamp_viewport_center(dev);
  dt_dev_pixelpipe_change_zoom_main(dev);

  return 0;
}

float dt_masks_form_set_interaction_value(dt_develop_t *dev, const int group_id, const int formid,
                                          dt_masks_interaction_t interaction,
                                          float value, dt_masks_increment_t increment, int flow,
                                          dt_masks_form_gui_t *mask_gui, dt_iop_module_t *module)
{
  if(IS_NULL_PTR(dev)) return NAN;

  if(interaction == DT_MASKS_INTERACTION_OPACITY)
  {
    /* Read, apply the increment, write back -- all by identity. The write API owns the
     * copy-on-write, so nothing here holds a row across the mutation. */
    dt_masks_member_t member = { 0 };
    if(dt_masks_group_get_member(dev, group_id, formid, &member) != DT_MASKS_OK) return NAN;

    const float target = dt_masks_apply_increment(member.opacity, value, increment, flow);
    const dt_masks_result_t result = dt_masks_group_set_member_opacity(dev, group_id, formid, target, &member);
    if(result != DT_MASKS_OK && result != DT_MASKS_UNCHANGED) return NAN;

    dt_toast_log(_("Opacity: %3.2f%%"), member.opacity * 100.f);
    return member.opacity;
  }

  dt_masks_form_t *target_form = dt_masks_get_from_id(dev, formid);
  if(IS_NULL_PTR(target_form) || !target_form->functions
     || !target_form->functions->set_interaction_value) return NAN;

  // The shape's own geometry is refcounted like any other dt_masks_form_t (an undo/redo
  // snapshot can share it) -- touch it before the vtable call below mutates form->points in
  // place, or a shared shape's geometry changes behind the back of a snapshot that still
  // references it. Safe to re-touch on every call: target_form is re-resolved by formid each
  // time, never cached across slider drag steps.
  target_form = dt_masks_cow_touch(dev, target_form);

  const float result = target_form->functions->set_interaction_value(target_form, interaction, value, increment,
                                                                     flow, mask_gui, module);
  if(isnan(result)) return NAN;
  dt_masks_form_update_gravity_center(dev, target_form);
  return result;
}

const char * _get_mask_plugin(dt_masks_form_t *mask_form)
{
  // Internal masks are used by spots removal and retouch modules
  if(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
    return "spots";
  // Regular all-purpose masks
  else
    return "masks";
}

float dt_masks_apply_increment(float current, float amount, dt_masks_increment_t increment, int flow)
{
  switch(increment)
  {
    case DT_MASKS_INCREMENT_SCALE:
      return current * powf(amount, (float)flow);
    case DT_MASKS_INCREMENT_OFFSET:
      return current + amount * (float)flow;
    case DT_MASKS_INCREMENT_ABSOLUTE:
    default:
      return amount;
  }
}

float dt_masks_apply_increment_precomputed(float current, float amount, float scale_amount, float offset_amount,
                                           dt_masks_increment_t increment)
{
  switch(increment)
  {
    case DT_MASKS_INCREMENT_SCALE:
      return current * scale_amount;
    case DT_MASKS_INCREMENT_OFFSET:
      return current + offset_amount;
    case DT_MASKS_INCREMENT_ABSOLUTE:
    default:
      return amount;
  }
}

float dt_masks_get_set_conf_value(dt_masks_form_t *mask_form, char *feature, float new_value,
                                  float value_min, float value_max,
                                  dt_masks_increment_t increment, int flow)
{
  gchar *config_key = NULL;
  if(!strcmp(feature, "opacity"))
    config_key = g_strdup_printf("plugins/darkroom/%s_opacity", _get_mask_plugin(mask_form));
  else
    config_key = g_strdup_printf("plugins/darkroom/%s/%s/%s",
                                 _get_mask_plugin(mask_form), dt_masks_type_name(mask_form->type), feature);

  if(!g_strcmp0(feature, "rotation")) flow = (flow > 1) ? (flow - 1) * 5 : flow;

  const float current_value = dt_conf_get_float(config_key);
  float updated_value = dt_masks_apply_increment(current_value, new_value, increment, flow);
  if(!g_strcmp0(feature, "rotation"))
  {
    // Ensure the rotation value stays within the interval [min, max)
    if(updated_value > value_max) updated_value = fmodf(updated_value, value_max);
    else if(updated_value < value_min)
      updated_value = value_max - fmodf(value_min - updated_value, value_max);
  }
  else updated_value = MAX(value_min, MIN(updated_value, value_max));

  dt_conf_set_float(config_key, updated_value);

  dt_free(config_key);
  return updated_value;
}

float dt_masks_get_set_conf_value_with_toast(dt_masks_form_t *mask_form, const char *feature, float amount,
                                             float value_min, float value_max,
                                             dt_masks_increment_t increment, int flow,
                                             const char *toast_fmt, float toast_scale)
{
  float value = dt_masks_get_set_conf_value(mask_form, (char *)feature, amount,
                                            value_min, value_max, increment, flow);
  if(!IS_NULL_PTR(toast_fmt) && toast_fmt[0] != '\0')
    dt_toast_log(toast_fmt, value * toast_scale);
  return value;
}

dt_masks_form_group_t *dt_masks_group_add_form_with_state(dt_develop_t *dev, dt_masks_form_t *group_form,
                                                          dt_masks_form_t *mask_form, const int parentid,
                                                          const dt_masks_state_t state, const float opacity)
{
  if(IS_NULL_PTR(group_form) || IS_NULL_PTR(mask_form)) return NULL;
  if(!(group_form->type & DT_MASKS_GROUP)) return NULL;

  /* Either the form being added is not a group, so there is no risk, or we walk it looking for a
   * reference back to this group. This is the guard the hand-rolled call sites skipped. */
  if((mask_form->type & DT_MASKS_GROUP) && _find_in_group(dev, mask_form, group_form->formid) != 0)
  {
    dt_control_log(_("Masks can not contain themselves"));
    return NULL;
  }

  dt_masks_form_group_t *group_entry = malloc(sizeof(dt_masks_form_group_t));
  if(IS_NULL_PTR(group_entry)) return NULL;

  group_entry->formid = mask_form->formid;
  group_entry->parentid = parentid;
  group_entry->state = state;
  group_entry->opacity = opacity;
  group_form->points = g_list_append(group_form->points, group_entry);

  /* The group's cached centre of gravity is now stale, and hit-testing reads it. The hand-rolled
   * sites left it stale. */
  dt_masks_form_update_gravity_center(dev, group_form);
  return group_entry;
}

dt_masks_form_group_t *dt_masks_group_add_form(dt_develop_t *dev, dt_masks_form_t *group_form, dt_masks_form_t *mask_form)
{
  if(IS_NULL_PTR(mask_form)) return NULL;
  if(IS_NULL_PTR(group_form)) return NULL;
  return dt_masks_group_add_form_with_state(dev, group_form, mask_form, group_form->formid,
                                            DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION,
                                            dt_conf_get_float("plugins/darkroom/masks/opacity"));
}

void dt_masks_group_ungroup(dt_develop_t *dev, dt_masks_form_t *dest_group, dt_masks_form_t *group_form)
{
  if(IS_NULL_PTR(group_form) || IS_NULL_PTR(dest_group)) return;
  if(!(group_form->type & DT_MASKS_GROUP) || !(dest_group->type & DT_MASKS_GROUP)) return;

  // dest_group->points is mutated below (also across recursive calls, which all receive this
  // same touched pointer by value): safe to self-touch here, unlike group_form which is only
  // ever read/recursed into, never mutated.
  dest_group = dt_masks_cow_touch(dev, dest_group);

  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    dt_masks_form_t *mask_form = dt_masks_get_from_id(dev, group_entry->formid);
    if(mask_form)
    {
      if(mask_form->type & DT_MASKS_GROUP)
      {
        dt_masks_group_ungroup(dev, dest_group, mask_form);
      }
      else
      {
        dt_masks_form_group_t *new_entry = (dt_masks_form_group_t *)malloc(sizeof(dt_masks_form_group_t));
        new_entry->formid = group_entry->formid;
        new_entry->parentid = group_entry->parentid;
        new_entry->state = group_entry->state;
        new_entry->opacity = group_entry->opacity;
        dest_group->points = g_list_append(dest_group->points, new_entry);
      }
    }
  }

  dt_masks_form_update_gravity_center(dev, dest_group);
}

/**
 * @brief Check whether any 2D point in pts[] lies inside the form points[].
 *
 * We use the ray casting algorithm for each tested point.
 *
 * @param pts Flat array of tested points [x0, y0, x1, y1, ...].
 * @param num_pts Number of tested points in pts.
 * @param points The array of form vertices.
 * @param points_start The starting index of the form vertices in the array.
 * @param points_count The total number of vertices in the form.
 * @return int Index of the first tested point found inside the form, -1 otherwise.
 */
int dt_masks_point_in_form_exact(const float *test_points, int test_point_count,
                                 const float *form_points, int form_points_start, int form_points_count,
                                 const dt_masks_skip_range_t *skips, int skip_count)
{
  if(IS_NULL_PTR(test_points) || test_point_count <= 0 || IS_NULL_PTR(form_points)) return -1;
  if(form_points_count <= 2 + form_points_start) return -1;
  if(form_points_start < 0 || form_points_start >= form_points_count) return -1;
  if(IS_NULL_PTR(skips)) skip_count = 0;

  const int start_index = form_points_start;
  // Once per call, not per finding: a corrupt input corrupts every test point alike, and the
  // point of reporting is a greppable line, not a flood.
  gboolean reported_bad_skip = FALSE;
  gboolean reported_trapped_walk = FALSE;

  for(int test_index = 0; test_index < test_point_count; test_index++)
  {
    int intersection_count = 0;
    const float point_x = test_points[test_index * 2];
    const float point_y = test_points[test_index * 2 + 1];
    int visited_points = 0;

    for(int i = form_points_start, next = start_index + 1; i < form_points_count;)
    {
      if(next < start_index || next >= form_points_count) break;
      if(++visited_points > form_points_count - start_index + 1)
      {
        /* Unreachable over a well-formed stream: the walk visits each index at most once and
         * wraps exactly once. Tripping it means the input is corrupt, and the crossing count
         * below is then garbage. This used to be silent -- both historical bugs of the cut
         * mechanism shipped as exactly this silence. */
        if(!reported_trapped_walk)
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[masks] point_in_form walk trapped after %d visits of %d points -- corrupt"
                   " outline or skip ranges; hit-test result is unreliable\n",
                   visited_points, form_points_count - form_points_start);
          reported_trapped_walk = TRUE;
        }
        break;
      }

      /* Out-of-band self-intersection cuts: on reaching one, close the contour with a chord to
       * its resume point. Only a skip that moves the walk STRICTLY FORWARD is followed -- a
       * backward one would re-walk the span just left until the cap above fires, which is the
       * cycle the old in-band encoding actually produced once. Such a range is a producer bug:
       * ignore it and say so. */
      gboolean jumped = TRUE;
      int hops = 0;
      while(jumped && hops <= skip_count)
      {
        jumped = FALSE;
        for(int s = 0; s < skip_count; s++)
        {
          if(next != skips[s].jump_from) continue;
          if(skips[s].resume_at <= skips[s].jump_from || skips[s].resume_at >= form_points_count)
          {
            if(!reported_bad_skip)
            {
              dt_print(DT_DEBUG_ALWAYS,
                       "[masks] skip range [%d -> %d] does not move forward within %d points --"
                       " ignoring it; the producer is broken\n",
                       skips[s].jump_from, skips[s].resume_at, form_points_count);
              reported_bad_skip = TRUE;
            }
          }
          else
          {
            next = skips[s].resume_at;
            jumped = TRUE;
            hops++;
          }
          break;
        }
      }
      if(next >= form_points_count) break;

      const float y1 = form_points[i * 2 + 1];
      const float y2 = form_points[next * 2 + 1];

      if(isnan(form_points[next * 2]))
      {
        /* NOTHING should reach this. dt_masks_get_points_border() guarantees the outline holds
         * finite geometry, and everything a consumer must not use travels beside it in the
         * exclusion list. Both shapes of sentinel are therefore reported: a NaN x with a finite
         * y was the in-band jump encoding, an index smuggled through a coordinate, and decoding
         * one silently is how issue #1313 shipped; a bare NaN,NaN was a close-the-contour
         * marker, which no builder emits any longer. This walk is also reached with buffers
         * that never went through that entry point, which is why the check stays at all. */
        if(!reported_bad_skip)
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[masks] non-finite outline sample at %d of %d (y %s) -- the buffer should"
                   " hold geometry only; treating the outline as ended\n", next, form_points_count,
                   isnan(y2) ? "also NaN" : "finite, i.e. the old in-band jump encoding");
          reported_bad_skip = TRUE;
        }
        break;
        if(next == start_index) break;
        next = start_index;
        continue;
      }

      if(((point_y <= y2 && point_y > y1) || (point_y >= y2 && point_y < y1))
         && (form_points[i * 2] > point_x))
        intersection_count++;

      if(next == start_index) break;
      i = next++;
      // loop
      if(next >= form_points_count) next = start_index;
    }

    if(intersection_count & 1) return test_index;
  }

  return -1;
}

/**
 * @brief Select or clear the current mask form, notifying the owning module if needed.
 *
 * Passing NULL clears the selection.
 */
void dt_masks_select_form(dt_develop_t *dev, struct dt_iop_module_t *module, dt_masks_form_t *selected_form)
{
  const int selected_formid = IS_NULL_PTR(selected_form) ? 0 : selected_form->formid;

  if(IS_NULL_PTR(module) && selected_formid == 0 && !IS_NULL_PTR(dev))
    module = dev->gui_module;

  if(!IS_NULL_PTR(module) && module->masks_selection_changed)
    module->masks_selection_changed(module, selected_formid);
}

/**
 * @brief Decide initial source positioning mode for clone masks.
 *
 * Uses key modifiers to choose absolute vs. relative positioning, and stores
 * the reference position in preview coordinates.
 * The current implementation caches that reference in absolute output-image coordinates.
 */
void dt_masks_set_source_pos_initial_state(dt_masks_form_gui_t *mask_gui, const uint32_t key_state)
{
  if(dt_modifier_is(key_state, GDK_SHIFT_MASK | DT_PRIMARY_MASK))
    mask_gui->source_pos_type = DT_MASKS_SOURCE_POS_ABSOLUTE;
  else if(dt_modifier_is(key_state, GDK_SHIFT_MASK))
    mask_gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
  else
    fprintf(stderr, "[dt_masks_set_source_pos_initial_state] unknown state for setting masks position type\n");

  // both source types record an absolute position,
  // for the relative type, the first time is used the position is recorded,
  // the second time a relative position is calculated based on that one
  mask_gui->pos_source[0] = mask_gui->pos[0];
  mask_gui->pos_source[1] = mask_gui->pos[1];
}

/**
 * @brief Initialize the clone source position based on current GUI state.
 *
 * Handles first-time relative positioning, existing relative offsets, and
 * absolute coordinates. Updates mask_form->source accordingly.
 * `mask_gui->rel_pos` is the normalized output-image
 * cursor, while `mask_gui->pos_source` stores either an absolute output-image
 * position or an absolute output-image delta depending on the current source mode.
 */
void dt_masks_set_source_pos_initial_value(dt_masks_form_gui_t *mask_gui, dt_masks_form_t *mask_form)
{
  dt_develop_t *dev = mask_gui->dev;
  const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(dev);
  const float raw_width = geometry.raw_width;
  const float raw_height = geometry.raw_height;

  const float xx = mask_gui->pos[0];
  const float yy = mask_gui->pos[1];

  // if this is the first time the relative pos is used
  if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    // if it has not been defined by the user, set some default
    if(mask_gui->pos_source[0] == -1.0f && mask_gui->pos_source[1] == -1.0f)
    {
      if(mask_form->functions && mask_form->functions->initial_source_pos)
      {
        mask_form->functions->initial_source_pos(dev, raw_width, raw_height,
                                                 &mask_gui->pos_source[0], &mask_gui->pos_source[1]);
      }
      else
        fprintf(stderr, "[dt_masks_set_source_pos_initial_value] unsupported masks type when calculating source position initial value\n");

      // set offset to form->source
      mask_form->source[0] = mask_gui->pos[0] + mask_gui->pos_source[0];
      mask_form->source[1] = mask_gui->pos[1] + mask_gui->pos_source[1];
      dt_dev_coordinates_image_abs_to_raw_abs(dev, mask_form->source, 1);
      // normalize backbuf points
      dt_dev_coordinates_raw_abs_to_raw_norm(dev, mask_form->source, 1);

    }
    else
    {
      // if a position was defined by the user, use the absolute value the first time
      float source_points[2] = { mask_gui->pos_source[0], mask_gui->pos_source[1] };
      dt_dev_coordinates_image_abs_to_raw_norm(dev, source_points, 1);

      mask_form->source[0] = source_points[0];
      mask_form->source[1] = source_points[1];

      mask_gui->pos_source[0] = mask_gui->pos_source[0] - xx;
      mask_gui->pos_source[1] = mask_gui->pos_source[1] - yy;
    }

    mask_gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE;
  }
  else if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    // original pos was already defined and relative value calculated, just use it
    mask_form->source[0] = mask_gui->pos[0] + mask_gui->pos_source[0];
    mask_form->source[1] = mask_gui->pos[1] + mask_gui->pos_source[1];
    dt_dev_coordinates_image_abs_to_raw_norm(dev, mask_form->source, 1);
  }
  else if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // an absolute position was defined by the user
    float source_points[2] = { mask_gui->pos_source[0], mask_gui->pos_source[1] };
    dt_dev_coordinates_image_abs_to_raw_norm(dev, source_points, 1);

    mask_form->source[0] = source_points[0];
    mask_form->source[1] = source_points[1];
  }
  else
    fprintf(stderr, "[dt_masks_set_source_pos_initial_value] unknown source position type\n");
}

/**
 * @brief Compute preview-space source position for drawing the clone indicator.
 *
 * This uses the stored source positioning mode and can follow the cursor while adding.
 */
void dt_masks_calculate_source_pos_origin(dt_masks_form_gui_t *mask_gui, const float initial_xpos,
                                         const float initial_ypos, const float xpos, const float ypos,
                                         float *pos_x, float *pos_y, const int adding)
{
  float source_x = 0.0f;
  float source_y = 0.0f;
  const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(mask_gui->dev);
  const float raw_width = geometry.raw_width;
  const float raw_height = geometry.raw_height;
  if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    source_x = xpos + mask_gui->pos_source[0];
    source_y = ypos + mask_gui->pos_source[1];
  }
  else if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    if(mask_gui->pos_source[0] == -1.0f && mask_gui->pos_source[1] == -1.0f)
    {
      const dt_masks_form_t *visible_form = dt_masks_get_visible_form(mask_gui->dev);
      if(!IS_NULL_PTR(visible_form) && visible_form->functions && visible_form->functions->initial_source_pos)
      {
        visible_form->functions->initial_source_pos(mask_gui->dev, raw_width, raw_height, &source_x, &source_y);
        source_x += xpos;
        source_y += ypos;
      }
      else
        fprintf(stderr, "[dt_masks_calculate_source_pos_origin] unsupported masks type when calculating source position value\n");
    }
    else
    {
      source_x = mask_gui->pos_source[0];
      source_y = mask_gui->pos_source[1];
    }
  }
  else if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // if the user is actually adding, the mask follow the cursor
    if(adding)
    {
      source_x = xpos + mask_gui->pos_source[0] - initial_xpos;
      source_y = ypos + mask_gui->pos_source[1] - initial_ypos;
    }
    else
    {
      // if not added yet set the start position
      source_x = mask_gui->pos_source[0];
      source_y = mask_gui->pos_source[1];
    }
  }
  else
    fprintf(stderr, "[dt_masks_calculate_source_pos_origin] unknown source position type for setting source position value\n");

  *pos_x = source_x;
  *pos_y = source_y;
}

/**
 * @brief Compute rotation angle (degrees) around a center using an anchor point.
 *
 * `anchor`, `center`, and `mask_gui->delta` are absolute output-image
 * coordinates. The angle accounts for possible axis inversion due to
 * distortion transforms.
 * Updates mask_gui->delta to store the last anchor position.
 */
float dt_masks_rotate_with_anchor(dt_develop_t *develop, const float anchor[2], const float center[2],
                                  dt_masks_form_gui_t *mask_gui)
{
  const float center_x = center[0];
  const float center_y = center[1];

  // get the current angle
  const float anchor_x = anchor[0];
  const float anchor_y = anchor[1];
  const float angle_current = atan2f(anchor_y - center_y, anchor_x - center_x);

  // get the previous angle
  const float delta_x = mask_gui->delta[0];
  const float delta_y = mask_gui->delta[1];
  const float angle_prev = atan2f(delta_y - center_y, delta_x - center_x);

  // calculate the angle difference an normalize to -180 to 180 degrees
  float delta_angle = angle_current - angle_prev;
  float angle = atan2f(sinf(delta_angle), cosf(delta_angle));

  // check if distortion inverts the axes
  float test_points[8] = { center_x, center_y, anchor_x , anchor_y,
                           center_x + 10.0f, center_y, center_x, center_y + 10.0f };
  dt_dev_coordinates_image_abs_to_raw_abs(develop, test_points, 4);
  float check_angle = atan2f(test_points[7] - test_points[1], test_points[6] - test_points[0])
                      - atan2f(test_points[5] - test_points[1], test_points[4] - test_points[0]);
  // Normalize to the range -180 to 180 degrees
  check_angle = atan2f(sinf(check_angle), cosf(check_angle));

  // Adjust the sign if the axes are inverted by distortion
  if(check_angle < 0.0f) angle = -angle;

  // Update the delta for the next frame (old position becomes the current one)
  mask_gui->delta[0] = anchor_x;
  mask_gui->delta[1] = anchor_y;

  return angle / M_PI * 180.0f;
}

/**
 * @brief Exit mask creation mode, restoring cursor visibility and resetting GUI state.
 *
 * @param mask_gui The GUI state of the mask form
 */
void dt_masks_creation_mode_quit(dt_masks_form_gui_t *mask_gui)
{
  if(IS_NULL_PTR(mask_gui)) return;

  mask_gui->creation = FALSE;
}

/**
 * @brief Enter mask creation mode for a given shape type.
 *
 * NOTE: this does quite the same as _menu_add_shape.
 */
gboolean dt_masks_creation_mode_enter(dt_develop_t *dev, dt_iop_module_t *module, const dt_masks_type_t type)
{
  if((type & DT_MASKS_ALL) == 0) return FALSE;
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->form_gui)) return FALSE;
  // we want to be sure that the iop has focus
  if(!IS_NULL_PTR(module)) dt_iop_request_focus(module);

  dt_masks_form_t *mask_form = dt_masks_create(type);
  if(IS_NULL_PTR(mask_form)) return FALSE;

  dt_masks_change_form_gui(dev, mask_form);
  dev->form_gui->creation = TRUE;
  dev->form_gui->creation_module = module;
  dev->form_gui->creation_type = type;
  dev->form_gui->creation_formids = NULL;
  dev->form_gui->creation_last_formid = 0;

  // Give focus to central view to allow using shortcuts for mask creation right after selecting a mask type in the manager
  gtk_widget_grab_focus(dt_gui_center_widget());
  return TRUE;
}

/**
 * @brief Apply a mask state operation on a group entry.
 *
 * Inverse toggles its flag, combine operations replace the combine bits.
 */
void dt_masks_group_entry_apply_operation(struct dt_masks_form_group_t *group_entry, const dt_masks_state_t apply_state)
{
  if(IS_NULL_PTR(group_entry)) return;

  // Apply Inverse
  if(apply_state == DT_MASKS_STATE_INVERSE)
    group_entry->state ^= DT_MASKS_STATE_INVERSE;
  
  else if((apply_state & DT_MASKS_STATE_IS_COMBINE_OP) != 0)
  {
    // Reset all and apply state
    group_entry->state = (group_entry->state & ~DT_MASKS_STATE_IS_COMBINE_OP) | apply_state;
  }
}
void dt_masks_set_edit_mode(struct dt_iop_module_t *module, dt_masks_edit_mode_t value)
{
  if(IS_NULL_PTR(module)) return;
  dt_iop_gui_blend_data_t *blend_data = module->gui ? (dt_iop_gui_blend_data_t *)module->gui->blend_data : NULL;
  if(IS_NULL_PTR(blend_data)) return;

  dt_masks_form_t *group_form = NULL;
  dt_masks_form_t *mask_form = dt_masks_get_from_id(module->dev, module->blend_params->mask_id);
  if(value && !IS_NULL_PTR(mask_form))
  {
    group_form = dt_masks_create_ext(module->dev, DT_MASKS_GROUP);
    group_form->formid = 0;
    dt_masks_group_ungroup(module->dev, group_form, mask_form);
  }

  if(blend_data) blend_data->masks_shown = value;

  dt_masks_change_form_gui(module->dev, group_form);
  module->dev->form_gui->edit_mode = value;
  if(value && mask_form)
    dt_dev_masks_selection_change(module->dev, NULL, mask_form->formid, FALSE);
  else
    dt_dev_masks_selection_change(module->dev, NULL, 0, FALSE);

  if(blend_data->masks_support)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_edit),
                                 value == DT_MASKS_EDIT_OFF ? FALSE : TRUE);

  dt_control_queue_redraw_center();
}

int dt_masks_form_change_opacity(dt_develop_t *dev, dt_masks_form_t *mask_form, int parent_id, int scroll_up,
                                 const int flow)
{
  if(IS_NULL_PTR(mask_form)) return 0;

  // Read, apply the increment, write back by identity. The write API owns the copy-on-write, so
  // neither half of this holds a row pointer across the other -- which is what the previous
  // touch-then-resolve-then-mutate-in-place version had to get right by hand.
  dt_masks_member_t member = { 0 };
  if(dt_masks_group_get_member(dev, parent_id, mask_form->formid, &member) != DT_MASKS_OK) return 0;

  const float amount = scroll_up ? 0.02f : -0.02f;
  const float target = dt_masks_apply_increment(member.opacity, amount, DT_MASKS_INCREMENT_OFFSET, flow);

  const dt_masks_result_t result = dt_masks_group_set_member_opacity(dev, parent_id, mask_form->formid,
                                                                     target, &member);
  if(result != DT_MASKS_OK && result != DT_MASKS_UNCHANGED) return 0;

  dt_toast_log(_("Opacity: %3.2f%%"), member.opacity * 100.f);

  // UNCHANGED still counts as handled: this return value is what every shape's scrolled handler
  // gives back to say it consumed the event, so reporting 0 once the opacity is already pinned at
  // 0 or 1 would let that scroll step fall through and zoom the canvas instead.
  return (result == DT_MASKS_OK || result == DT_MASKS_UNCHANGED);
}



/* ------------------------------------------------------------------------------------- */
/* The headless half of the diagnostic renderer.
 *
 * It lives here, and not next to dt_masks_debug_rasterise() in masks_debug.c, for one reason:
 * compositing the overlay needs cairo, and src/develop is kept free of every toolkit type by
 * tools/check_module_boundaries.sh so the pixel and params engines stay portable. This file is
 * already the GUI half of masks -- and it is where dt_masks_events_post_expose_with(), the one
 * production drawing routine this calls, already lives. There is deliberately no second
 * drawing path: what a regression test looks at is what the darkroom paints. */
/** Rasterise @p form and paint it under the overlay as 8-bit grey. Its own function because
 * compositing a backdrop and drawing an overlay are two jobs, and nesting the pixel loop inside
 * the surface bookkeeping made both harder to follow. */
static void _paint_mask_backdrop(cairo_t *cr, dt_develop_t *dev, dt_masks_form_t *form,
                                 const int width, const int height)
{
  float *const mask = dt_masks_debug_rasterise(dev, form, width, height);
  if(IS_NULL_PTR(mask)) return;

  cairo_surface_t *grey = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  if(cairo_surface_status(grey) == CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_flush(grey);
    uint8_t *const pixels = cairo_image_surface_get_data(grey);
    const int stride = cairo_image_surface_get_stride(grey);
    for(int y = 0; y < height; y++)
    {
      uint32_t *const row = (uint32_t *)(pixels + (size_t)y * stride);
      for(int x = 0; x < width; x++)
      {
        const float v = mask[(size_t)y * width + x];
        const uint32_t g = (uint32_t)(CLAMPF(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        row[x] = (g << 16) | (g << 8) | g;
      }
    }
    cairo_surface_mark_dirty(grey);
    cairo_set_source_surface(cr, grey, 0, 0);
    cairo_paint(cr);
  }
  cairo_surface_destroy(grey);
  dt_free_align(mask);
}

gboolean dt_masks_debug_write_png(dt_develop_t *dev, dt_masks_form_t *form,
                                  const dt_masks_debug_request_t *request, const char *path)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(request) || IS_NULL_PTR(path)) return FALSE;
  if(IS_NULL_PTR(form)) form = dt_masks_get_visible_form(dev);
  if(IS_NULL_PTR(form)) return FALSE;

  int32_t raw_width = 0;
  int32_t raw_height = 0;
  if(!dt_dev_geometry_get_raw_size(dev, &raw_width, &raw_height) || raw_width <= 0 || raw_height <= 0)
    return FALSE;

  int width = request->width > 0 ? request->width : raw_width;
  int height = request->height > 0 ? request->height
                                   : (int)lrint((double)width * raw_height / (double)raw_width);
  if(width <= 0 || height <= 0) return FALSE;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surface);
    return FALSE;
  }
  cairo_t *cr = cairo_create(surface);

  if(request->backdrop != DT_MASKS_DEBUG_BACKDROP_TRANSPARENT)
  {
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);
  }

  if(request->backdrop == DT_MASKS_DEBUG_BACKDROP_RASTER)
    _paint_mask_backdrop(cr, dev, form, width, height);

  if(request->draw_overlay)
  {
    /* The overlay paints with the theme palette, which is filled from GTK at startup and is
     * therefore all-zero -- fully transparent -- with no GUI. Drawing would "succeed" and
     * produce an empty picture. Give every unset entry a visible fallback so a headless render
     * shows the same geometry the darkroom would; the exact hues are the theme's business, and
     * a diagnostic only needs to be legible. Production is untouched: with a GUI up, every
     * entry already has a non-zero alpha and nothing here applies. */
    GdkRGBA *const palette = dt_widget_colors();
    if(!IS_NULL_PTR(palette))
    {
      for(int i = 0; i < DT_GUI_COLOR_LAST; i++)
        if(palette[i].alpha <= 0.0) palette[i] = (GdkRGBA){ 1.0, 1.0, 1.0, 1.0 };
    }

    /* The overlay reads the visible form and the processed size off the dev, so both are
     * published here rather than passed -- this is the same state the darkroom holds while it
     * draws, which is the point: no second code path. */
    if(IS_NULL_PTR(dev->form_gui))
    {
      dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
      if(!IS_NULL_PTR(dev->form_gui)) dt_masks_init_form_gui(dev, dev->form_gui);
    }
    if(!IS_NULL_PTR(dev->form_gui))
    {
      dev->form_gui->dev = dev;
      dev->form_gui->form_visible = form;
      dev->form_gui->formid = 0;   // force the outline cache to rebuild for this render
      dt_dev_geometry_set_processed_size(dev, raw_width, raw_height);

      const dt_masks_overlay_transform_t transform
          = { .scale = (double)width / (double)raw_width, .offset_x = 0.0, .offset_y = 0.0 };
      int pw = 0, ph = 0;
      dt_dev_get_processed_size(dev, &pw, &ph);
      dt_print(DT_DEBUG_ALWAYS, "[masks debug] overlay: visible=%p processed=%dx%d scale=%.4f\n",
               (void *)dt_masks_get_visible_form(dev), pw, ph, transform.scale);
      dt_masks_events_post_expose_with(dev, NULL, cr, width, height, -1, -1, &transform);
    }
  }

  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const gboolean ok = (cairo_surface_write_to_png(surface, path) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(surface);

  if(!ok) dt_print(DT_DEBUG_ALWAYS, "[masks debug] could not write %s\n", path);
  return ok;
}

/**
 * @brief Append a shape's outline to @p cr as the COMPLEMENT of its exclusion list.
 *
 * @details Samples [@p first, @p last) are emitted as one sub-path per run between the spans in
 * @p skips, which are the places the offset curve doubles back on itself and must not be shown.
 *
 * There is one implementation of this walk because there was very nearly none: the encoding it
 * replaces blanked the excluded samples to NaN inside the point buffer, which each shape then
 * tested for in its own copy of the loop. That is an encoding hidden in a buffer of plain
 * floats -- invisible to any consumer that does not already know, and silently wrong for one
 * that forgets. Two consumers had forgotten: the polygon's outline drew its folds, and the
 * brush's per-node border handle lost both its dash and its drag wherever a blanked sample was
 * the nearest one.
 *
 * Emitting runs also makes a hole a hole. Skipping excluded samples with the pen down turns the
 * next one into a line_to, so each span comes out as a straight chord across the shape --
 * measured on issue #1313's brush at 50 to 137 pixels, one per node. A new sub-path per run
 * cannot do that, and stroking a path of several sub-paths costs nothing.
 *
 * @p skips must be sorted and disjoint (dt_masks_skip_ranges_build() guarantees it); pass NULL
 * and 0 for a shape with nothing to exclude.
 *
 * It lives here rather than in widgets/draw.h with the other drawing helpers because it needs
 * both cairo and the masks vocabulary, and a widget header must not inherit the latter -- that
 * would invert the layering. masks_gui.c already has both.
 */
/** Append samples [@p first, @p last) as ONE cairo sub-path, decimated to what the context can
 * show. A sub-path per run is what makes an excluded span absent rather than a shortcut across
 * it: skipping samples with the pen down turns the next one into a line_to, and the span comes
 * out as a straight chord. */
static inline void _emit_run(cairo_t *cr, const float *const points, const int first, const int last,
                             const double min_step2)
{
  double last_x = points[first * 2];
  double last_y = points[first * 2 + 1];
  cairo_move_to(cr, last_x, last_y);

  for(int i = first + 1; i < last; i++)
  {
    const double x = points[i * 2];
    const double y = points[i * 2 + 1];
    const double dx = x - last_x;
    const double dy = y - last_y;
    if((dx * dx + dy * dy) < min_step2) continue;
    cairo_line_to(cr, x, y);
    last_x = x;
    last_y = y;
  }

  /* the run's last sample always lands, so a run ends where the geometry does and not wherever
   * the decimation happened to stop */
  const double x = points[(last - 1) * 2];
  const double y = points[(last - 1) * 2 + 1];
  if(x != last_x || y != last_y) cairo_line_to(cr, x, y);
}

void dt_masks_draw_outline_runs(cairo_t *cr, const float *const points, const int first, const int last,
                                const dt_masks_skip_range_t *skips, const int skip_count)
{
  if(!cr || !points || first >= last) return;

  /* One line_to per sample at RAW resolution is several per device pixel; emit at the resolution
   * the context can actually show. See dt_draw_min_emit_step(). */
  const double min_step = dt_draw_min_emit_step(cr);
  const double min_step2 = min_step * min_step;

  const int count = IS_NULL_PTR(skips) ? 0 : skip_count;

  int at = first;
  int next = 0;

  while(at < last)
  {
    while(next < count && skips[next].resume_at <= at) next++;

    int run_end = last;
    if(next < count && skips[next].jump_from < last) run_end = MAX(skips[next].jump_from, at);

    if(run_end > at) _emit_run(cr, points, at, run_end, min_step2);

    if(next < count && skips[next].jump_from < last)
    {
      at = MAX(skips[next].resume_at, at + 1);
      next++;
    }
    else
      break;
  }
}

int dt_masks_preview_add_clone_source(dt_masks_form_gui_t *gui, dt_masks_preview_buffers_t *preview)
{
  float source_pos[2] = { 0.0f, 0.0f };
  dt_masks_calculate_source_pos_origin(gui, gui->pos[0], gui->pos[1], gui->pos[0], gui->pos[1],
                                       &source_pos[0], &source_pos[1], FALSE);
  const float center_source[2] = { source_pos[0] - gui->pos[0], source_pos[1] - gui->pos[1] };

  preview->source_points = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * preview->points_count, 0);
  if(IS_NULL_PTR(preview->source_points)) return 1;

  for(int i = 0; i < preview->points_count; i++)
  {
    preview->source_points[i * 2] = preview->points[i * 2] + center_source[0];
    preview->source_points[i * 2 + 1] = preview->points[i * 2 + 1] + center_source[1];
  }

  return 0;
}


int dt_masks_points_shift_to_source(dt_develop_t *dev, const dt_iop_module_t *module,
                                    float **points, int *points_count,
                                    const float xs, const float ys, const int first_shifted)
{
  // every distortion that happens BEFORE the module: the TARGET outline in module input reference
  if(!dt_dev_distort_transform_gui(dev, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                   *points, *points_count))
    goto error;

  // the source anchor, taken to the same reference, gives the shift
  float pts[2] = { xs, ys };
  dt_dev_coordinates_raw_norm_to_raw_abs(dev, pts, 1);
  if(!dt_dev_distort_transform_gui(dev, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 1))
    goto error;

  {
    const float dx = pts[0] - (*points)[0];
    const float dy = pts[1] - (*points)[1];

    // a shape with handle points in its header keeps them where they are and takes the anchor
    // verbatim; one whose point 0 is just the centre lets the loop below carry it
    if(first_shifted > 0)
    {
      (*points)[0] = pts[0];
      (*points)[1] = pts[1];
    }

    __OMP_PARALLEL_FOR_SIMD__(if(*points_count > 100) aligned(points:64))
    for(int i = first_shifted; i < *points_count; i++)
    {
      (*points)[i * 2] += dx;
      (*points)[i * 2 + 1] += dy;
    }
  }

  // and the distortions AFTER the module: the SOURCE outline in final image reference
  if(!dt_dev_distort_transform_gui(dev, module->iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL,
                                   *points, *points_count))
    goto error;

  return 0;

error:
  dt_pixelpipe_cache_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 1;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
