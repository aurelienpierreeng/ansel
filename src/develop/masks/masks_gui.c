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
#include "develop/masks.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "gui/actions/menu.h"
#include "gui/draw.h"
#include "dtgtk/paint.h"

#include <math.h>

typedef struct dt_masks_gui_interaction_slider_t
{
  dt_masks_form_group_t *form_group;
  dt_masks_form_gui_t *gui;
  dt_iop_module_t *module;
  dt_masks_interaction_t interaction;
  dt_masks_increment_t increment;
  float last_value;
  GtkWidget *slider;
} dt_masks_gui_interaction_slider_t;

static void _masks_gui_interaction_apply_value(dt_masks_gui_interaction_slider_t *data, float value)
{
  if(!data || !data->form_group) return;
  const int saved_node_hovered = data->gui ? data->gui->node_hovered : -1;
  const gboolean saved_node_selected = data->gui ? data->gui->node_selected : FALSE;
  const int saved_node_selected_idx = data->gui ? data->gui->node_selected_idx : -1;

  if(data->increment == DT_MASKS_INCREMENT_ABSOLUTE) // aka opacity
  {
    dt_masks_form_set_interaction_value(data->form_group, data->interaction, value,
                                        DT_MASKS_INCREMENT_ABSOLUTE, 1, data->gui, data->module);
    return;
  }

  const float delta = value - data->last_value;
  if(fabsf(delta) < 1e-6f) return;

  // Slider value is a log2 scale factor in [-3;3], so apply the delta in log space.
  const float mult = (data->interaction == DT_MASKS_INTERACTION_HARDNESS) ? -1.f : 1.f;
  const float scale = exp2f(mult * delta);
  if(data->gui)
  {
    data->gui->node_hovered = -1;
    data->gui->node_selected = FALSE;
    data->gui->node_selected_idx = -1;
  }
  dt_masks_form_set_interaction_value(data->form_group, data->interaction, scale,
                                      DT_MASKS_INCREMENT_SCALE, 1, data->gui, data->module);
  if(data->gui)
  {
    data->gui->node_hovered = saved_node_hovered;
    data->gui->node_selected = saved_node_selected;
    data->gui->node_selected_idx = saved_node_selected_idx;
  }
  data->last_value = value;
}

static void _masks_gui_menu_item_block_activate(GtkWidget *widget, gpointer user_data)
{
  g_signal_stop_emission_by_name(widget, "activate");
}

static gboolean _masks_gui_menu_item_forward_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_masks_gui_interaction_slider_t *data = (dt_masks_gui_interaction_slider_t *)user_data;
  if(!data || !data->slider) return FALSE;

  GdkEvent *copy = gdk_event_copy(event);
  if(!copy) return FALSE;

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
  if(!data || !data->form_group) return;

  _masks_gui_interaction_apply_value(data, dt_bauhaus_slider_get(widget));
}

static GtkWidget *_masks_gui_add_interaction_slider(GtkWidget *menu, const char *label, dt_masks_form_group_t *form_group,
                                                    dt_masks_interaction_t interaction, dt_masks_increment_t increment,
                                                    float min, float max, float step, float value, int digits,
                                                    const char *format, float factor,
                                                    dt_masks_form_gui_t *gui, dt_iop_module_t *module)
{
  GtkWidget *menu_item = gtk_menu_item_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_set_can_focus(menu_item, FALSE);
  g_signal_connect(G_OBJECT(menu_item), "activate",
                   G_CALLBACK(_masks_gui_menu_item_block_activate), NULL);
  gtk_widget_add_events(menu_item, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                   | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);

  GtkWidget *slider = dt_bauhaus_slider_new_with_range(darktable.bauhaus, module ? DT_GUI_MODULE(module) : NULL,
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
  gtk_widget_set_size_request(menu_item, -1, DT_PIXEL_APPLY_DPI(48));

  dt_masks_gui_interaction_slider_t *data = g_malloc0(sizeof(dt_masks_gui_interaction_slider_t));
  data->form_group = form_group;
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

static void _masks_gui_remove_form_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  if(!gui) return;
  dt_masks_form_t *forms = dt_masks_get_visible_form(darktable.develop);
  if(!forms) return;

  if(gui->group_selected >= 0)
  {
    // Delete shape from current group
    dt_masks_form_group_t *fpt = dt_masks_form_get_selected_group(forms, gui);
    if(!fpt) return;
    dt_iop_module_t *module = darktable.develop->gui_module;
    if(!module) return;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel)
      dt_masks_gui_delete(module, sel, gui, fpt->parentid);

    dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, fpt->formid, fpt->parentid, DT_MASKS_EVENT_REMOVE);
  }
}

void _masks_gui_delete_node_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  if(!gui) return;
  dt_masks_form_t *forms = dt_masks_get_visible_form(darktable.develop);
  if(!forms) return;

  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;

  if(gui->creation)
  {
    // Minimum points to create a polygon
    if(gui->node_dragging < 1)
    {
      dt_masks_form_cancel_creation(module, gui);
      return;
    }
    dt_masks_form_t *sel = dt_masks_get_visible_form(darktable.develop);
    if(sel)
      dt_masks_remove_node(module, sel, 0, gui, 0, gui->node_dragging);
    gui->node_dragging -= 1;
  }
  else if(gui->group_selected >= 0)
  {
    // Delete shape from current group

    dt_masks_form_group_t *fpt = dt_masks_form_get_selected_group(forms, gui);
    if(!fpt) return;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel)
      dt_masks_remove_node(module, sel, fpt->parentid, gui, gui->group_selected, gui->node_hovered);

    dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
  }
}

static void _masks_gui_cancel_creation_callback(GtkWidget *menu, gpointer user_data)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  dt_iop_module_t *module = darktable.develop->gui_module;
  dt_masks_form_cancel_creation(module, gui);
}

static void _masks_move_up_down_callback(gpointer user_data, const int up)
{
  dt_masks_form_gui_t *gui = (dt_masks_form_gui_t *)user_data;
  if(!gui) return;
  if(gui->group_selected < 0) return;

  dt_iop_module_t *module = darktable.develop->gui_module;
  if(!module) return;

  dt_masks_form_t *forms = dt_masks_get_visible_form(darktable.develop);
  if(!forms) return;
  dt_masks_form_group_t *fpt = dt_masks_form_get_selected_group(forms, gui);
  if(!fpt) return;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, fpt->parentid);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;

  dt_masks_form_move(grp, fpt->formid, up);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, fpt->formid, fpt->parentid, DT_MASKS_EVENT_CHANGE);
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
  if(!gui || !menu) return;

  const guint form_pos = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(menu), "form_pos"));
  const dt_masks_state_t state_op = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu), "state_op"));
  // Advert the user if it will have no effect
  if(form_pos == 0 && (state_op & DT_MASKS_STATE_IS_COMBINE_OP) != 0)
  {
    dt_control_log(_("Applying a boolean operation has no effect on the first shape of a group.\n"
         "Move it to at least the 2nd position if you need to use boolean operations"));
  }

  dt_masks_form_group_t *form_op = (dt_masks_form_group_t *)g_object_get_data(G_OBJECT(menu), "op_form");
  if(!form_op) return;

  apply_operation(form_op, state_op);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, form_op->formid, form_op->parentid, DT_MASKS_EVENT_UPDATE);
}

#define masks_gtk_menu_item_new_bold(label, selected, state, icon)                                        \
{                                                                                                         \
  gchar *op_title = g_strdup(label);                                                                      \
  gchar *op_label = g_strdup_printf("%s", op_title);                                                      \
  menu_item = ctx_gtk_check_menu_item_new_with_markup_and_pixbuf(op_label, icon,                          \
                                                                    sub_menu,                             \
                                                                    _masks_operation_callback, gui,       \
                                                                    (selected != 0),                      \
                                                                    ((state) == DT_MASKS_STATE_INVERSE)); \
  g_free(op_label);                                                                                       \
  g_free(op_title);                                                                                       \
  g_object_set_data(G_OBJECT(menu_item), "state_op", GINT_TO_POINTER(state));                             \
  g_object_set_data(G_OBJECT(menu_item), "op_form", op_form);                                             \
  g_object_set_data(G_OBJECT(menu_item), "form_pos", GINT_TO_POINTER(form_pos));                          \
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
  dt_masks_form_t *grp = formgroup ? dt_masks_get_from_id(darktable.develop, formgroup->parentid) : NULL;
  if(grp && (grp->type & DT_MASKS_GROUP))
    op_form = dt_masks_form_group_from_parentid(grp->formid, form->formid);
  if(!op_form) return NULL;

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
        form_name = g_strconcat(form_name, _("group"), NULL);
        break;
      default:
        g_free(form_name); // Erase the "New " prefix
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
  const dt_masks_state_t state = op_form->state & DT_MASKS_STATE_IS_COMBINE_OP;
  const gboolean has_inverse = (op_form->state & DT_MASKS_STATE_INVERSE) != 0;
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

  const gboolean draw_icon = form_pos > 0;
  gchar *title = g_strdup_printf("<b><big>%s%s</big></b>", item_str, form_name);
  GtkWidget *menu_item = ctx_gtk_menu_item_new_with_markup_and_pixbuf(title, (draw_icon) ? icon : NULL, menu, NULL, gui);
  gtk_widget_set_sensitive(menu_item, FALSE);
  g_free(item_str);
  g_free(title);
  g_free(form_name);

  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

  // Shape specific menu items
  if(form && form->functions && form->functions->populate_context_menu)
    if(form->functions->populate_context_menu(menu, form, gui, pzx, pzy))
    {
      sep = gtk_separator_menu_item_new();
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
    }


  /* Module specific */
  {
    dt_iop_module_t *module = darktable.develop->gui_module;
    if(module && module->populate_masks_context_menu)
      if(module->populate_masks_context_menu(module, menu, form->formid, pzx, pzy))
      {
        sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
      }
  }

  int i = 0;
  for(GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *f = (dt_masks_form_t *)forms->data;
    fprintf(stderr, "%2d) dt_masks_form_t %s,\tID %d,\t\"%s\"\n", i,
                                f->type & DT_MASKS_CIRCLE ? "circle" :
                                f->type & DT_MASKS_ELLIPSE ? "ellipse" :
                                f->type & DT_MASKS_POLYGON ? "polygon" :
                                f->type & DT_MASKS_BRUSH ? "brush" :
                                f->type & DT_MASKS_GRADIENT ? "gradient" :
                                f->type & DT_MASKS_GROUP ? "group" : "unknown", f->formid, f->name);


    if(f->type & DT_MASKS_GROUP)
    {
      fprintf(stderr, "   ||\n");
      for(GList *gf = f->points; gf; gf = g_list_next(gf))
      {
        dt_masks_form_group_t *group_pt = (dt_masks_form_group_t *)gf->data;
        fprintf(stderr, "   |-> dt_masks_form_group_t \tID %d,\tparentid: %d,\t state: %s%s%s%s%s%s%s%s\n", group_pt->formid, group_pt->parentid, group_pt->state & DT_MASKS_STATE_INVERSE ? _("inverse ") : "",
                                                        group_pt->state & DT_MASKS_STATE_USE ? _("use ") : "",
                                                        group_pt->state & DT_MASKS_STATE_NONE ? _("none ") : "",
                                                        group_pt->state & DT_MASKS_STATE_SHOW ? _("show ") : "",
                                                        group_pt->state & DT_MASKS_STATE_UNION ? _("union ") : "",
                                                        group_pt->state & DT_MASKS_STATE_INTERSECTION ? _("intersection ") : "",
                                                        group_pt->state & DT_MASKS_STATE_DIFFERENCE ? _("difference ") : "",
                                                        group_pt->state & DT_MASKS_STATE_EXCLUSION ? _("exclusion ") : "");
      }
    }
    i++;
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "----\n"
                  "form: %d\n"
                  "form_visible: %d\n"
                  "parentid: %d\n\n", form->formid,
                  dt_masks_get_visible_form(darktable.develop)
                    ? dt_masks_get_visible_form(darktable.develop)->formid
                    : -1,
                  formgroup ? formgroup->parentid : -1);

  /*  Operation */

  if(!gui->creation && !(form->type & DT_MASKS_IS_RETOUCHE) && (op_form) && gui->form_selected)
  {
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Operation"), menu, NULL, gui);
    GtkWidget *sub_menu = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sub_menu), "dt-masks-context-menu");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item), sub_menu);

    masks_gtk_menu_item_new_bold(_("Invert"), (op_form->state & DT_MASKS_STATE_INVERSE), DT_MASKS_STATE_INVERSE,
                                 op_icon[DT_MASKS_STATE_INVERSE]);
    sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(sub_menu), sep);
    masks_gtk_menu_item_new_bold(_("Union"), (op_form->state & DT_MASKS_STATE_UNION), DT_MASKS_STATE_UNION,
                                 op_icon[DT_MASKS_STATE_UNION]);
    masks_gtk_menu_item_new_bold(_("Intersection"), (op_form->state & DT_MASKS_STATE_INTERSECTION), DT_MASKS_STATE_INTERSECTION,
                                 op_icon[DT_MASKS_STATE_INTERSECTION]);
    masks_gtk_menu_item_new_bold(_("Difference"), (op_form->state & DT_MASKS_STATE_DIFFERENCE), DT_MASKS_STATE_DIFFERENCE,
                                 op_icon[DT_MASKS_STATE_DIFFERENCE]);
    masks_gtk_menu_item_new_bold(_("Exclusion"), (op_form->state & DT_MASKS_STATE_EXCLUSION), DT_MASKS_STATE_EXCLUSION,
                                 op_icon[DT_MASKS_STATE_EXCLUSION]);

    sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(sub_menu), sep);
  }

  // Common menu items
  if(!gui->creation && (gui->form_selected || gui->node_selected) && op_form)
  {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    const float opacity = dt_masks_form_get_interaction_value(op_form, DT_MASKS_INTERACTION_OPACITY);

    _masks_gui_add_interaction_slider(menu, _("Size"), op_form, DT_MASKS_INTERACTION_SIZE,
                                      DT_MASKS_INCREMENT_SCALE, -4.f, 4.0f, 0.01f, 0.0f, 2, "x", 1.0f,
                                      gui, darktable.develop->gui_module);
    _masks_gui_add_interaction_slider(menu, _("Hardness"), op_form, DT_MASKS_INTERACTION_HARDNESS,
                                      DT_MASKS_INCREMENT_SCALE, -4.f, 4.0f, 0.01f, 0.0f, 2, "x", 1.0f,
                                      gui, darktable.develop->gui_module);
    if(gui->form_selected)
      _masks_gui_add_interaction_slider(menu, _("Opacity"), op_form, DT_MASKS_INTERACTION_OPACITY,
                                        DT_MASKS_INCREMENT_ABSOLUTE, 0.0f, 1.0f, 0.01f,
                                        isfinite(opacity) ? opacity : 1.0f, 3, "%", 100.0f,
                                        gui, darktable.develop->gui_module);

    sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
  }

  if(!gui->creation && gui->form_selected)
  {
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Move up"), menu, _masks_moveup_callback, gui);
    gtk_widget_set_sensitive(menu_item, (form_pos > 0));
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Move down"), menu, _masks_movedown_callback, gui);
    gtk_widget_set_sensitive(menu_item, (form_pos < list_length - 1));

    sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
  }

  if(gui->creation)
  {
    menu_item = ctx_gtk_menu_item_new_with_markup(_("Cancel"), menu, _masks_gui_cancel_creation_callback, gui);
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
      menu_item = ctx_gtk_menu_item_new_with_markup(_("Remove form"), menu, _masks_gui_remove_form_callback, gui);
      menu_item_set_fake_accel(menu_item, GDK_KEY_Delete, 0);
      gtk_widget_set_sensitive(menu_item, gui->form_selected >= 0);
    }
  }

  for(size_t k = 0; k < G_N_ELEMENTS(op_icon); k++)
    g_clear_object(&op_icon[k]);
  g_clear_object(&composed_icon);

  gtk_widget_show_all(menu);
  return menu;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
