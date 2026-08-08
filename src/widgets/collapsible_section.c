/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "widgets/collapsible_section.h"

#include "widgets/expander.h"
#include "widgets/label.h"
#include "widgets/paint.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"
#include "widgets/togglebutton.h"

static void _collapsible_set_states(dt_gui_collapsible_section_t *cs, gboolean active)
{
  if(active)
  {
    // We don't apply the GTK_STATE_SELECTED flag to the container here because it would
    // be inherited by all children, which would mess up the state of checkboxes and togglebuttons.
    dt_gui_add_class(GTK_WIDGET(cs->expander), "active");
  }
  else
  {
    gtk_widget_set_state_flags(GTK_WIDGET(cs->expander), GTK_STATE_FLAG_NORMAL, TRUE);
    dt_gui_remove_class(GTK_WIDGET(cs->expander), "active");
  }
}

static void _collapsible_container_show(GtkWidget *widget, gpointer user_data)
{
  /* Called whenever the container receives a "show" event, including from gtk_widget_show_all().
   * If the toggle is not active the section should remain collapsed, so we re-hide the container
   * immediately. By the time this fires, show_all has already recursed into the children and
   * set their visible flags, so a later expand will find all children ready to display. */
  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle)))
    gtk_widget_hide(widget);
}

static void _coeffs_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), active);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(cs->toggle), dtgtk_cairo_paint_solid_arrow,
                               (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  dt_widget_store_bool(cs->confname, active);
  _collapsible_set_states(cs, active);
}

static void _coeffs_expander_click(GtkWidget *widget, GdkEventButton *e, gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return;

  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), !active);
  _collapsible_set_states(cs, !active);
}

void dt_gui_update_collapsible_section(dt_gui_collapsible_section_t *cs)
{
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(cs->toggle), dtgtk_cairo_paint_solid_arrow,
                               (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), active);

  if(active)
    gtk_widget_show(GTK_WIDGET(cs->container));
  else
    gtk_widget_hide(GTK_WIDGET(cs->container));

  _collapsible_set_states(cs, active);
}

void dt_gui_hide_collapsible_section(dt_gui_collapsible_section_t *cs)
{
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), FALSE);
  gtk_widget_hide(GTK_WIDGET(cs->container));
  _collapsible_set_states(cs, FALSE);
}

void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname, const char *label,
                                    GtkBox *parent, GtkPackType pack)
{
  const gboolean expanded = dt_widget_stored_bool(confname);

  cs->confname = confname;
  cs->parent = parent;

  // collapsible section header
  GtkWidget *destdisp_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  GtkWidget *header_evb = gtk_event_box_new();
  cs->label = dt_ui_section_label_new(label);
  dt_gui_add_class(destdisp_head, "dt_section_expander");
  gtk_container_add(GTK_CONTAINER(header_evb), cs->label);

  cs->toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow,
                                      (expanded ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), expanded);
  dt_gui_add_class(cs->toggle, "dt_ignore_fg_state");

  cs->container = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING));
  gtk_widget_set_name(GTK_WIDGET(cs->container), "collapsible");
  gtk_box_pack_start(GTK_BOX(destdisp_head), header_evb, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(destdisp_head), cs->toggle, FALSE, FALSE, 0);

  cs->expander = dtgtk_expander_new(destdisp_head, GTK_WIDGET(cs->container));
  /* gtk_widget_show_all() on the parent (called by lib modules after gui_init) recurses into
   * the container and would override the collapsed state set by dtgtk_expander_set_expanded.
   * Connect to "show" so we can re-apply the correct visibility right after show_all touches it. */
  g_signal_connect(G_OBJECT(cs->container), "show",
                   G_CALLBACK(_collapsible_container_show), (gpointer)cs);
  // Pack at the requested side so callers control ordering at insertion time.
  if(pack == GTK_PACK_START)
    gtk_box_pack_start(GTK_BOX(cs->parent), cs->expander, FALSE, FALSE, 0);
  else
    gtk_box_pack_end(GTK_BOX(cs->parent), cs->expander, FALSE, FALSE, 0);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), expanded);
  gtk_widget_set_name(cs->expander, "collapse-block");

  g_signal_connect(G_OBJECT(cs->toggle), "toggled",
                   G_CALLBACK(_coeffs_button_changed),  (gpointer)cs);

  g_signal_connect(G_OBJECT(header_evb), "button-release-event",
                   G_CALLBACK(_coeffs_expander_click),
                   (gpointer)cs);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
