/*
    This file is part of darktable,
    Copyright (C) 2011-2012 Henrik Andersson.
    Copyright (C) 2011-2012, 2014 johannes hanika.
    Copyright (C) 2012, 2014 Jérémy Rosen.
    Copyright (C) 2012 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2013 Simon Spannagel.
    Copyright (C) 2012, 2014-2019 Tobias Ellinghaus.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018-2022 Pascal Obry.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2022 Aldric Renaudin.
    Copyright (C) 2019-2021, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019, 2021 Hanno Schwalm.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2022 Nicolas Auffray.
    Copyright (C) 2020 parafin.
    Copyright (C) 2020 Sergey Salnikov.
    Copyright (C) 2021-2022 Diederik Ter Rahe.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Marco.
    Copyright (C) 2021 Mark-64.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2023 Luca Zulberti.
    
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
#include "common/iop_order.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

#include "modulegroups.h"

typedef struct dt_lib_modulegroups_t
{
  uint32_t current;
  GtkWidget *notebook;
  GtkWidget *page_pipeline;
  GtkWidget *page_basic;
  GtkWidget *page_repair;
  GtkWidget *page_sharpness;
  GtkWidget *page_effects;
  GtkWidget *page_technical;
  GtkWidget *page_all;
  GtkWidget *section_color;
  GtkWidget *section_film;
  GtkWidget *section_tones;
  GtkWidget *container_color;
  GtkWidget *container_film;
  GtkWidget *container_tones;
  GtkWidget *drag_highlight;
  dt_iop_module_t *drag_source;
} dt_lib_modulegroups_t;

static dt_lib_module_t *g_modulegroups_module = NULL;
static dt_lib_modulegroups_t *g_modulegroups_data = NULL;
static const dt_lib_modulegroup_t _modulegroups_pages[] = { DT_MODULEGROUP_ACTIVE_PIPE, DT_MODULEGROUP_TONES,
                                                            DT_MODULEGROUP_REPAIR, DT_MODULEGROUP_SHARPNESS,
                                                            DT_MODULEGROUP_EFFECTS, DT_MODULEGROUP_TECHNICAL,
                                                            DT_MODULEGROUP_NONE };
typedef enum dt_modulegroups_dnd_target_t
{
  DT_MODULEGROUPS_DND_TARGET_IOP = 0
} dt_modulegroups_dnd_target_t;

static const GtkTargetEntry _modulegroups_target_list[] = {
  { "iop", GTK_TARGET_SAME_APP, DT_MODULEGROUPS_DND_TARGET_IOP }
};
static const guint _modulegroups_n_targets = G_N_ELEMENTS(_modulegroups_target_list);

/* toggle button callback */
static void _lib_modulegroups_toggle(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
/* helper function to update iop module view depending on group */
static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self);

static void _lib_modulegroups_signal_set(gpointer instance, gpointer module, gpointer user_data);
static void _lib_modulegroups_module_moved(gpointer instance, gpointer user_data);
static void _lib_modulegroups_refresh(gpointer instance, gpointer user_data);

static gboolean _focus_next_module();
static gboolean _focus_previous_module();
static gboolean _focus_next_control();
static gboolean _focus_previous_control();
static gboolean _is_module_in_history(const dt_iop_module_t *module);
static void _modulegroups_ensure_page_widgets(dt_lib_module_t *self);
static GtkWidget *_modulegroups_target_container(const dt_lib_modulegroups_t *d, const dt_iop_module_t *module);
static void _modulegroups_setup_drag_source(dt_lib_module_t *self, dt_iop_module_t *module);
static gboolean _modulegroups_reorder_target(GtkWidget *target);

/**
 * @brief Hide every modulegroups-owned page before showing the active one.
 *
 * We always start a refresh by hiding all pages, then we show only the page
 * matching the current tab once the modules have been reparented and reordered.
 *
 * @param d Modulegroups runtime data.
 */
static void _modulegroups_hide_pages(const dt_lib_modulegroups_t *d)
{
  if(!d) return;
  if(d->page_pipeline) gtk_widget_hide(d->page_pipeline);
  if(d->page_basic) gtk_widget_hide(d->page_basic);
  if(d->page_repair) gtk_widget_hide(d->page_repair);
  if(d->page_sharpness) gtk_widget_hide(d->page_sharpness);
  if(d->page_effects) gtk_widget_hide(d->page_effects);
  if(d->page_technical) gtk_widget_hide(d->page_technical);
  if(d->page_all) gtk_widget_hide(d->page_all);
}

/**
 * @brief Align the basic-tab section labels with the module expander margins.
 *
 * The section label border is drawn on the label widget itself. To make that
 * border line start and end exactly where module boxes do, we copy the current
 * expander margins from the first available module widget.
 *
 * @param d Modulegroups runtime data.
 */
static void _modulegroups_sync_section_label_margins(dt_lib_modulegroups_t *d)
{
  if(!d || !darktable.develop) return;
  if(!d->section_color || !d->section_film || !d->section_tones) return;

  GtkWidget *reference = NULL;
  for(const GList *modules = g_list_first(darktable.develop->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(!dt_iop_is_hidden(module) && module->expander)
    {
      reference = module->expander;
      break;
    }
  }
  if(!reference) return;

  GtkBorder margin = { 0 };
  GtkStyleContext *context = gtk_widget_get_style_context(reference);
  gtk_style_context_get_margin(context, gtk_style_context_get_state(context), &margin);

  GtkWidget *labels[] = { d->section_color, d->section_film, d->section_tones };
  for(size_t i = 0; i < G_N_ELEMENTS(labels); i++)
  {
    gtk_widget_set_margin_start(labels[i], margin.left);
    gtk_widget_set_margin_end(labels[i], margin.right);
    gtk_widget_set_margin_top(labels[i], margin.top);
    gtk_widget_set_margin_bottom(labels[i], margin.bottom);
  }
}

/**
 * @brief Remove all drag-and-drop visual feedback from module headers.
 *
 * Modulegroups owns the containers hosting the module expanders, so it also
 * owns the temporary drop markers shown while reordering the list.
 *
 * @param d Modulegroups runtime data.
 */
static void _modulegroups_clear_drop_state(dt_lib_modulegroups_t *d)
{
  if(d && d->drag_highlight)
  {
    gtk_drag_unhighlight(d->drag_highlight);
    d->drag_highlight = NULL;
  }

  if(!darktable.develop) return;

  /* Walk every module and clear the before/after classes that motion handlers add. */
  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(!module->expander) continue;
    dt_gui_remove_class(module->expander, "iop_drop_after");
    dt_gui_remove_class(module->expander, "iop_drop_before");
  }
}

/**
 * @brief Append visible module expanders in display order from a page subtree.
 *
 * The basic tab nests its sections inside extra boxes, so drag-and-drop must
 * recurse through the visible widget tree and keep only actual module
 * expanders.
 *
 * @param widget Current widget in the page subtree.
 * @param widgets Output list collecting expanders in display order.
 */
static void _modulegroups_append_visible_expanders(GtkWidget *widget, GList **widgets)
{
  if(!GTK_IS_WIDGET(widget) || !gtk_widget_is_visible(widget)) return;

  if(g_object_get_data(G_OBJECT(widget), "dt-module"))
  {
    *widgets = g_list_append(*widgets, widget);
    return;
  }

  if(!GTK_IS_CONTAINER(widget)) return;

  /* Recurse over the current page subtree to preserve the visual order seen by the user. */
  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  for(GList *child = children; child; child = g_list_next(child))
    _modulegroups_append_visible_expanders(GTK_WIDGET(child->data), widgets);
  g_list_free(children);
}

/**
 * @brief Find the module header under the current drop position.
 *
 * The y coordinate is relative to the page widget receiving the drop event.
 * We translate each visible module expander into that same coordinate space so
 * reordering keeps working across nested section containers.
 *
 * @param page Visible modulegroups page handling the drag.
 * @param y Drop coordinate in the page reference frame.
 * @param module_src Drag source module.
 * @return Destination module under the drop position, or NULL.
 */
static dt_iop_module_t *_modulegroups_get_dnd_dest_module(GtkWidget *page, const gint y,
                                                          dt_iop_module_t *module_src)
{
  if(!GTK_IS_WIDGET(page) || !module_src) return NULL;

  GtkAllocation source_allocation = { 0 };
  gtk_widget_get_allocation(module_src->header, &source_allocation);
  const int y_slop = source_allocation.height / 2;
  gboolean after_src = TRUE;
  dt_iop_module_t *module_dest = NULL;

  GList *children = NULL;
  _modulegroups_append_visible_expanders(page, &children);

  /* Walk the displayed headers in page coordinates and pick the closest valid insertion anchor. */
  for(GList *l = children; l; l = g_list_next(l))
  {
    GtkWidget *w = GTK_WIDGET(l->data);
    if(w == module_src->expander) after_src = FALSE;

    int widget_x = 0;
    int widget_y = 0;
    if(!gtk_widget_translate_coordinates(w, page, 0, 0, &widget_x, &widget_y)) continue;

    GtkAllocation allocation = { 0 };
    gtk_widget_get_allocation(w, &allocation);
    if((after_src && y <= widget_y + y_slop)
       || (!after_src && y <= widget_y + allocation.height + y_slop))
    {
      module_dest = (dt_iop_module_t *)g_object_get_data(G_OBJECT(w), "dt-module");
      break;
    }
  }

  g_list_free(children);
  return module_dest;
}

static void _modulegroups_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data);
static void _modulegroups_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data);
static void _modulegroups_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                                        GtkSelectionData *selection_data, guint info, guint time,
                                        gpointer user_data);
static gboolean _modulegroups_drag_drop(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                        guint time, gpointer user_data);
static gboolean _modulegroups_drag_motion(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                          guint time, gpointer user_data);
static void _modulegroups_drag_data_received(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                             GtkSelectionData *selection_data, guint info, guint time,
                                             gpointer user_data);
static void _modulegroups_drag_leave(GtkWidget *widget, GdkDragContext *dc, guint time, gpointer user_data);

static void _modulegroups_move_widget(GtkWidget *widget, GtkWidget *target)
{
  if(!GTK_IS_WIDGET(widget) || !GTK_IS_BOX(target)) return;

  GtkWidget *parent = gtk_widget_get_parent(widget);
  if(parent == target) return;

  g_object_ref(widget);
  if(GTK_IS_CONTAINER(parent)) gtk_container_remove(GTK_CONTAINER(parent), widget);
  gtk_box_pack_start(GTK_BOX(target), widget, FALSE, FALSE, 0);
  g_object_unref(widget);
}

static void _modulegroups_track_widget(GtkWidget **slot, GtkWidget *widget)
{
  *slot = widget;
  g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)slot);
}

static void _modulegroups_ensure_page_widgets(dt_lib_module_t *self)
{
  if(!self || !self->data || !darktable.gui || !darktable.gui->ui) return;

  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(d->page_pipeline && d->page_basic && d->page_repair && d->page_sharpness
     && d->page_effects && d->page_technical && d->page_all
     && d->section_color && d->section_film && d->section_tones
     && d->container_color && d->container_film && d->container_tones)
    return;

  GtkBox *root = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  if(!root) return;

  _modulegroups_track_widget(&d->page_pipeline, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(root, d->page_pipeline, FALSE, FALSE, 0);

  _modulegroups_track_widget(&d->page_basic, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  _modulegroups_track_widget(&d->section_color, dt_ui_section_label_new(_("color")));
  _modulegroups_track_widget(&d->container_color, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  _modulegroups_track_widget(&d->section_film, dt_ui_section_label_new(_("film")));
  _modulegroups_track_widget(&d->container_film, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  _modulegroups_track_widget(&d->section_tones, dt_ui_section_label_new(_("tones")));
  _modulegroups_track_widget(&d->container_tones, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(d->page_basic), d->section_color, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->page_basic), d->container_color, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->page_basic), d->section_film, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->page_basic), d->container_film, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->page_basic), d->section_tones, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(d->page_basic), d->container_tones, FALSE, FALSE, 0);
  gtk_box_pack_start(root, d->page_basic, FALSE, FALSE, 0);

  _modulegroups_track_widget(&d->page_repair, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(root, d->page_repair, FALSE, FALSE, 0);
  _modulegroups_track_widget(&d->page_sharpness, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(root, d->page_sharpness, FALSE, FALSE, 0);
  _modulegroups_track_widget(&d->page_effects, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(root, d->page_effects, FALSE, FALSE, 0);
  _modulegroups_track_widget(&d->page_technical, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(root, d->page_technical, FALSE, FALSE, 0);
  _modulegroups_track_widget(&d->page_all, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(root, d->page_all, FALSE, FALSE, 0);

  GtkWidget *pages[] = {
    d->page_pipeline, d->page_basic, d->page_repair, d->page_sharpness,
    d->page_effects, d->page_technical, d->page_all
  };
  for(size_t i = 0; i < G_N_ELEMENTS(pages); i++)
  {
    gtk_drag_dest_set(pages[i], 0, _modulegroups_target_list, _modulegroups_n_targets, GDK_ACTION_COPY);
    g_signal_connect(pages[i], "drag-data-received", G_CALLBACK(_modulegroups_drag_data_received), self);
    g_signal_connect(pages[i], "drag-drop", G_CALLBACK(_modulegroups_drag_drop), self);
    g_signal_connect(pages[i], "drag-motion", G_CALLBACK(_modulegroups_drag_motion), self);
    g_signal_connect(pages[i], "drag-leave", G_CALLBACK(_modulegroups_drag_leave), self);
  }

  gtk_widget_show_all(d->page_pipeline);
  gtk_widget_show_all(d->page_basic);
  gtk_widget_show_all(d->page_repair);
  gtk_widget_show_all(d->page_sharpness);
  gtk_widget_show_all(d->page_effects);
  gtk_widget_show_all(d->page_technical);
  gtk_widget_show_all(d->page_all);
  gtk_widget_hide(d->page_pipeline);
  gtk_widget_hide(d->page_basic);
  gtk_widget_hide(d->page_repair);
  gtk_widget_hide(d->page_sharpness);
  gtk_widget_hide(d->page_effects);
  gtk_widget_hide(d->page_technical);
  gtk_widget_hide(d->page_all);
}

/**
 * @brief Return the GtkBox currently hosting a module for the active tab.
 *
 * The basic tab splits its modules into three subgroup containers, while the
 * other tabs each own a single page box.
 *
 * @param d Modulegroups runtime data.
 * @param module Module whose container is requested.
 * @return Target GtkWidget container, or NULL when the current tab should not host it.
 */
static GtkWidget *_modulegroups_target_container(const dt_lib_modulegroups_t *d, const dt_iop_module_t *module)
{
  if(!d || !module) return NULL;

  switch(d->current)
  {
    case DT_MODULEGROUP_ACTIVE_PIPE:
      return d->page_pipeline;

    case DT_MODULEGROUP_NONE:
      return d->page_all;

    case DT_MODULEGROUP_TONES:
      if(module->default_group() == DT_MODULEGROUP_COLOR) return d->container_color;
      if(module->default_group() == DT_MODULEGROUP_FILM) return d->container_film;
      if(module->default_group() == DT_MODULEGROUP_TONES) return d->container_tones;
      return NULL;

    case DT_MODULEGROUP_REPAIR:
      return d->page_repair;

    case DT_MODULEGROUP_SHARPNESS:
      return d->page_sharpness;

    case DT_MODULEGROUP_EFFECTS:
      return d->page_effects;

    case DT_MODULEGROUP_TECHNICAL:
      return d->page_technical;

    default:
      return NULL;
  }
}

/**
 * @brief Attach the module drag source handlers once to an expander widget.
 *
 * Modulegroups owns module reordering in the darkroom panel, so it also owns
 * the drag source registration for every visible expander.
 *
 * @param self Modulegroups lib module.
 * @param module Module whose expander should become draggable.
 */
static void _modulegroups_setup_drag_source(dt_lib_module_t *self, dt_iop_module_t *module)
{
  if(!self || !module || !module->expander) return;

  GtkWidget *widget = module->expander;
  g_object_set_data(G_OBJECT(widget), "dt-module", module);

  if(g_object_get_data(G_OBJECT(widget), "modulegroups-dnd")) return;

  gtk_drag_source_set(widget, GDK_BUTTON1_MASK, _modulegroups_target_list, _modulegroups_n_targets, GDK_ACTION_COPY);
  g_signal_connect(widget, "drag-begin", G_CALLBACK(_modulegroups_drag_begin), self);
  g_signal_connect(widget, "drag-data-get", G_CALLBACK(_modulegroups_drag_data_get), self);
  g_signal_connect(widget, "drag-end", G_CALLBACK(_modulegroups_drag_end), self);
  g_object_set_data(G_OBJECT(widget), "modulegroups-dnd", GINT_TO_POINTER(TRUE));
}

/**
 * @brief Reorder one page or subgroup container to match reverse pipeline order.
 *
 * We walk the whole pipeline from last to first and keep only expanders whose
 * current parent is the requested container. This keeps the GUI order
 * consistent regardless of the active tab layout.
 *
 * @param target Page or subgroup container to reorder.
 * @return TRUE when at least one visible module ended up in that container.
 */
static gboolean _modulegroups_reorder_target(GtkWidget *target)
{
  if(!GTK_IS_WIDGET(target) || !darktable.develop) return FALSE;

  gboolean has_visible = FALSE;
  int position = 0;

  /* Walk the whole pipeline in reverse order and keep only the modules currently parented here. */
  for(GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(dt_iop_is_hidden(module) || !module->expander || !gtk_widget_get_visible(module->expander)) continue;
    if(gtk_widget_get_parent(module->expander) != target) continue;

    gtk_box_reorder_child(GTK_BOX(target), module->expander, position++);
    has_visible = TRUE;
  }

  return has_visible;
}

static void _modulegroups_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_iop_module_t *module_src = (dt_iop_module_t *)g_object_get_data(G_OBJECT(widget), "dt-module");

  d->drag_source = module_src;
  _modulegroups_clear_drop_state(d);
  g_object_set_data(G_OBJECT(widget), "dt-module-dragged", GINT_TO_POINTER(TRUE));

  if(!module_src || !module_src->header) return;

  GdkWindow *window = gtk_widget_get_parent_window(module_src->header);
  if(!window) return;

  GtkAllocation allocation = { 0 };
  gtk_widget_get_allocation(module_src->header, &allocation);
  cairo_surface_t *surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(surface);

  dt_gui_add_class(module_src->header, "iop_drag_icon");
  gtk_widget_draw(module_src->header, cr);
  dt_gui_remove_class(module_src->header, "iop_drag_icon");

  cairo_surface_set_device_offset(surface, -allocation.width * darktable.gui->ppd / 2,
                                  -allocation.height * darktable.gui->ppd / 2);
  gtk_drag_set_icon_surface(context, surface);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

static void _modulegroups_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  _modulegroups_clear_drop_state(d);
  d->drag_source = NULL;
  g_object_set_data(G_OBJECT(widget), "dt-module-dragged", NULL);
}

static void _modulegroups_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                                        GtkSelectionData *selection_data, guint info, guint time,
                                        gpointer user_data)
{
  const guint number_data = 1;
  gtk_selection_data_set(selection_data, gdk_atom_intern("iop", TRUE), 32, (const guchar *)&number_data, 1);
}

static gboolean _modulegroups_drag_drop(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                        guint time, gpointer user_data)
{
  gtk_drag_get_data(widget, dc, gdk_atom_intern("iop", TRUE), time);
  return TRUE;
}

static gboolean _modulegroups_drag_motion(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                          guint time, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_iop_module_t *module_src = d->drag_source;
  if(!module_src) return FALSE;

  dt_iop_module_t *module_dest = _modulegroups_get_dnd_dest_module(widget, y, module_src);
  gboolean can_move = FALSE;
  _modulegroups_clear_drop_state(d);

  if(module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
      can_move = dt_ioppr_check_can_move_after_iop(darktable.develop->iop, module_src, module_dest);
    else
      can_move = dt_ioppr_check_can_move_before_iop(darktable.develop->iop, module_src, module_dest);
  }

  if(!can_move)
  {
    gdk_drag_status(dc, 0, time);
    return FALSE;
  }

  if(module_src->iop_order < module_dest->iop_order)
    dt_gui_add_class(module_dest->expander, "iop_drop_after");
  else
    dt_gui_add_class(module_dest->expander, "iop_drop_before");

  d->drag_highlight = module_dest->expander;
  gtk_drag_highlight(module_dest->expander);
  gdk_drag_status(dc, GDK_ACTION_COPY, time);
  return TRUE;
}

static void _modulegroups_drag_data_received(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                             GtkSelectionData *selection_data, guint info, guint time,
                                             gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_iop_module_t *module_src = d->drag_source;
  dt_iop_module_t *module_dest = _modulegroups_get_dnd_dest_module(widget, y, module_src);
  gboolean moved = FALSE;

  if(module_src && module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
      moved = dt_iop_gui_move_module_after(module_src, module_dest, "_modulegroups_drag_data_received");
    else
      moved = dt_iop_gui_move_module_before(module_src, module_dest, "_modulegroups_drag_data_received");
  }

  gtk_drag_finish(dc, TRUE, FALSE, time);
  _modulegroups_clear_drop_state(d);
  d->drag_source = NULL;

  if(!moved) return;
}

static void _modulegroups_drag_leave(GtkWidget *widget, GdkDragContext *dc, guint time, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  _modulegroups_clear_drop_state(d);
}

static int _modulegroups_page_count()
{
  return (int)G_N_ELEMENTS(_modulegroups_pages);
}

static int _modulegroups_page_from_group(uint32_t group)
{
  if(group == DT_MODULEGROUP_FILM || group == DT_MODULEGROUP_COLOR) group = DT_MODULEGROUP_TONES;

  for(int page = 0; page < _modulegroups_page_count(); page++)
    if(_modulegroups_pages[page] == group) return page;

  return 0;
}

static uint32_t _modulegroups_group_from_page(int page)
{
  if(page < 0) return _modulegroups_pages[_modulegroups_page_count() - 1];
  if(page >= _modulegroups_page_count()) return _modulegroups_pages[0];
  return _modulegroups_pages[page];
}

const char *name(struct dt_lib_module_t *self)
{
  return _("modulegroups");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "darkroom", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}


/* this module should always be shown without expander */
int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 999;
}

int dt_iop_get_group(const dt_iop_module_t *module)
{
  return 1 << (module->default_group());
}

int _modulegroups_cycle_tabs(int user_set_group)
{
  int group;
  if(user_set_group < 0)
  {
    // cycle to the end
    group = _modulegroups_page_count() - 1;
  }
  else if(user_set_group >= _modulegroups_page_count())
  {
    // cycle to the beginning
    group = 0;
  }
  else
  {
    group = user_set_group;
  }
  return group;
}

static uint32_t _modulegroups_get_current_group()
{
  if(g_modulegroups_data && g_modulegroups_data->current < DT_MODULEGROUP_SIZE)
    return g_modulegroups_data->current;

  return DT_MODULEGROUP_ACTIVE_PIPE;
}

static void _modulegroups_set_current_group(uint32_t group)
{
  if(!g_modulegroups_module || !g_modulegroups_data) return;
  if(group >= DT_MODULEGROUP_SIZE) return;
  if(group == DT_MODULEGROUP_FILM || group == DT_MODULEGROUP_COLOR) group = DT_MODULEGROUP_TONES;
  if(g_modulegroups_data->current == group) return;

  g_modulegroups_data->current = group;
  if(GTK_IS_NOTEBOOK(g_modulegroups_data->notebook))
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_modulegroups_data->notebook), _modulegroups_page_from_group(group));

  _lib_modulegroups_update_iop_visibility(g_modulegroups_module);
}

static gboolean _modulegroups_switch_tab_next(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                              GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;
  if(!dev) return FALSE;

  dt_iop_module_t *focused = dev->gui_module;
  if(focused) dt_iop_gui_set_expanded(focused, FALSE, TRUE);

  const int current = _modulegroups_page_from_group(_modulegroups_get_current_group());
  _modulegroups_set_current_group(_modulegroups_group_from_page(_modulegroups_cycle_tabs(current + 1)));
  dt_iop_request_focus(NULL);
  return TRUE;
}

static gboolean _modulegroups_switch_tab_previous(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                                  GdkModifierType modifier, gpointer data)
{
  dt_develop_t *dev = (dt_develop_t *)data;
  if(!dev) return FALSE;

  dt_iop_module_t *focused = dev->gui_module;
  if(focused) dt_iop_gui_set_expanded(focused, FALSE, TRUE);

  const int current = _modulegroups_page_from_group(_modulegroups_get_current_group());
  _modulegroups_set_current_group(_modulegroups_group_from_page(_modulegroups_cycle_tabs(current - 1)));
  dt_iop_request_focus(NULL);

  return TRUE;
}

static gboolean _lib_modulegroups_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_x, delta_y;

  // We will accumulate scrolls here
  static int scrolls = 0;

  if(dt_gui_get_scroll_unit_deltas(event, &delta_x, &delta_y))
  {
    int current = _modulegroups_page_from_group(_modulegroups_get_current_group());
    int future = 0;
    if(delta_x > 0. || delta_y > 0.)
      future = current + 1;
    else if(delta_x < 0. || delta_y < 0.)
      future = current - 1;

    if(future < 0 || future > _modulegroups_page_count() - 1)
    {
      // We reached the end of tabs. Allow cycling through, but add a little inertia to fight.
      // This is to ensure user really wants to cycle through.
      if(scrolls > 4)
      {
        scrolls = 0;
      }
      else
      {
        // Do nothing but increment
        scrolls++;
        return FALSE;
      }
    }

    _modulegroups_set_current_group(_modulegroups_group_from_page(_modulegroups_cycle_tabs(future)));
    dt_iop_request_focus(NULL);
  }

  return TRUE;
}


static void _focus_module(dt_iop_module_t *module)
{
  if(module && dt_iop_gui_module_is_visible(module))
  {
    dt_iop_request_focus(module);
    dt_iop_gui_set_expanded(module, TRUE, TRUE);
    darktable.gui->scroll_to[1] = module->expander;
  }
  else
  {
    // we reached the extremity of the list.
    dt_iop_request_focus(NULL);
  }
}

static dt_iop_module_t *_module_from_active_group(dt_iop_module_t *mod, uint32_t current_group)
{
  if(!mod) return NULL; // that should never happen

  if(dt_iop_gui_module_is_visible(mod) &&
    (dt_is_module_in_group(mod, current_group) || _is_module_in_history(mod)))
    return mod;
  else
    return NULL;
}

/* WARNING: first/last refer to pipeline nodes order, which is reversed compared to GUI order. */
static dt_iop_module_t *_find_first_visible_module()
{
  uint32_t current_group = _modulegroups_get_current_group();

  for(GList *module = g_list_first(darktable.develop->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)module->data;
    if(_module_from_active_group(mod, current_group)) return mod;
  }
  return NULL;
}

static dt_iop_module_t *_find_last_visible_module()
{
  uint32_t current_group = _modulegroups_get_current_group();

  for(GList *module = g_list_last(darktable.develop->iop); module; module = g_list_previous(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)module->data;
    if(_module_from_active_group(mod, current_group)) return mod;
  }
  return NULL;
}

/* WARNING: next/previous refer to GUI order, which is reversed pipeline order
* in a "layer over" logic: first pipeline node is at the GUI bottom.
*/
static gboolean _focus_previous_module()
{
  dt_iop_module_t *focused = darktable.develop->gui_module;
  if(IS_NULL_PTR(focused))
  {
    _focus_module(_find_first_visible_module());
  }
  else
  {
    dt_iop_gui_set_expanded(focused, FALSE, TRUE);
    _focus_module(dt_iop_gui_get_next_visible_module(focused));
  }

  return TRUE;
}

static gboolean _focus_next_module()
{
  dt_iop_module_t *focused = darktable.develop->gui_module;
  if(IS_NULL_PTR(focused))
  {
    _focus_module(_find_last_visible_module());
  }
  else
  {
    dt_iop_gui_set_expanded(focused, FALSE, TRUE);
    _focus_module(dt_iop_gui_get_previous_visible_module(focused));
  }

  return TRUE;
}

static gboolean _is_valid_widget(GtkWidget *widget)
{
  // The parent will always be a GtkBox
  GtkWidget *parent = gtk_widget_get_parent(widget);
  GtkWidget *grandparent = gtk_widget_get_parent(parent);
  GType type = G_OBJECT_TYPE(grandparent);

  gboolean visible_parent = TRUE;

  if(type == GTK_TYPE_NOTEBOOK)
  {
    // Find the page in which the current widget is and try to show it
    gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(grandparent), parent);
    if(page_num > -1)
      gtk_notebook_set_current_page(GTK_NOTEBOOK(grandparent), page_num);
    else
      visible_parent = FALSE;
  }
  else if(type == GTK_TYPE_STACK)
  {
    // Stack pages are enabled based on user parameteters,
    // so if not visible, then do nothing.
    GtkWidget *visible_child = gtk_stack_get_visible_child(GTK_STACK(grandparent));
    if(visible_child != parent) visible_parent = FALSE;
  }

  return gtk_widget_is_visible(widget) && gtk_widget_is_sensitive(widget)
         && visible_parent;
}


// Because Gtk can't focus on invisible widgets without errors
// (and weird behaviour on user's end), getting the first widget in the list is not enough.
static GList *_find_next_visible_widget(GList *widgets)
{
  for(GList *first = widgets; first; first = g_list_next(first))
  {
    GtkWidget *widget = (GtkWidget *)first->data;
    if(_is_valid_widget(widget)) return first;
  }
  return NULL;
}


static GList *_find_previous_visible_widget(GList *widgets)
{
  for(GList *last = widgets; last; last = g_list_previous(last))
  {
    GtkWidget *widget = (GtkWidget *)last->data;
    if(_is_valid_widget(widget)) return last;
  }
  return NULL;
}

static void _focus_widget(GtkWidget *widget)
{
  gtk_widget_grab_focus(widget);
  darktable.gui->has_scroll_focus = widget;
}


static gboolean _focus_next_control()
{
  dt_iop_module_t *focused = darktable.develop->gui_module;
  dt_gui_module_t *m = DT_GUI_MODULE(focused);
  if(!focused || !m->widget_list) return FALSE;

  GtkWidget *current_widget = darktable.gui->has_scroll_focus;
  GList *first_item = _find_next_visible_widget(g_list_first(m->widget_list));

  if(!current_widget && first_item)
  {
    // No active widget, start by the first
    _focus_widget(GTK_WIDGET(first_item->data));
  }
  else
  {
    GList *current_item = g_list_find(m->widget_list, current_widget);
    GList *next_item = _find_next_visible_widget(g_list_next(current_item));

    // Select the next visible item, if any
    if(next_item)
      _focus_widget(GTK_WIDGET(next_item->data));
    // Cycle back to the beginning
    else if(first_item)
      _focus_widget(GTK_WIDGET(first_item->data));
  }

  return TRUE;
}

static gboolean _focus_previous_control()
{
  dt_iop_module_t *focused = darktable.develop->gui_module;
  dt_gui_module_t *m = DT_GUI_MODULE(focused);
  if(!focused || !m->widget_list) return FALSE;

  GtkWidget *current_widget = darktable.gui->has_scroll_focus;
  GList *last_item = _find_previous_visible_widget(g_list_last(m->widget_list));

  if(!current_widget && last_item)
  {
    // No active widget, start by the last
    _focus_widget(GTK_WIDGET(last_item->data));
  }
  else
  {
    GList *current_item = g_list_find(m->widget_list, current_widget);
    GList *previous_item = _find_previous_visible_widget(g_list_previous(current_item));

    // Select the previous item, if any
    if(previous_item)
      _focus_widget(GTK_WIDGET(previous_item->data));
    // Cycle back to the end
    else if(last_item)
      _focus_widget(GTK_WIDGET(last_item->data));
  }

  return TRUE;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)g_malloc0(sizeof(dt_lib_modulegroups_t));
  self->data = (void *)d;
  const int conf_group = dt_conf_get_int("plugins/darkroom/groups");
  d->current = (conf_group >= 0 && conf_group < DT_MODULEGROUP_SIZE)
                   ? (uint32_t)conf_group
                   : DT_MODULEGROUP_ACTIVE_PIPE;
  if(d->current == DT_MODULEGROUP_FILM || d->current == DT_MODULEGROUP_COLOR)
    d->current = DT_MODULEGROUP_TONES;
  g_modulegroups_module = self;
  g_modulegroups_data = d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_widget_set_name(self->widget, "modules-tabs");

  /* Tabs */
  d->notebook = GTK_WIDGET(gtk_notebook_new());
  char *labels[] = { _("Pipeline"), _("Basic"), _("Repair"), _("Sharpness"), _("Effects"), _("Technics"), _("All") };
  char *tooltips[]
      = { _("List all modules currently enabled in the reverse order of application in the pipeline."),
          _("Modules destined to adjust brightness, contrast and dynamic range, work with film scans, and perform color-grading."),
          _("Modules destined to repair and reconstruct noisy or missing pixels."),
          _("Modules destined to manipulate local contrast, sharpness and blur."),
          _("Modules applying special effects."),
          _("Technical modules that can be ignored in most situations."),
          _("All modules available in the software.") };

  for(int i = 0; i < _modulegroups_page_count(); i++)
  {
    GtkWidget *label = gtk_label_new(labels[i]);
    dt_gui_add_class(label, "dt_modulegroups_tab_label");
    gtk_widget_set_tooltip_text(label, tooltips[i]);
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(d->notebook), page, label);
    gtk_container_child_set(GTK_CONTAINER(d->notebook), page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  }
  gtk_notebook_set_current_page(GTK_NOTEBOOK(d->notebook), _modulegroups_page_from_group(d->current));
  gtk_notebook_popup_enable(GTK_NOTEBOOK(d->notebook));
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(d->notebook), TRUE);
  g_signal_connect(G_OBJECT(d->notebook), "switch_page", G_CALLBACK(_lib_modulegroups_toggle), self);
  g_signal_connect(G_OBJECT(d->notebook), "scroll-event", G_CALLBACK(_lib_modulegroups_scroll), self);
  gtk_widget_add_events(GTK_WIDGET(d->notebook), darktable.gui->scroll_mask);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->notebook), TRUE, TRUE, 0);
  gtk_widget_show_all(self->widget);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MODULEGROUPS_SET,
                                  G_CALLBACK(_lib_modulegroups_signal_set), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED,
                                  G_CALLBACK(_lib_modulegroups_module_moved), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                                  G_CALLBACK(_lib_modulegroups_refresh), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                                  G_CALLBACK(_lib_modulegroups_refresh), self);
  // History edits already trigger explicit modulegroups visibility updates through
  // DT_SIGNAL_DEVELOP_MODULEGROUPS_SET. Listening to generic history changes here
  // causes unnecessary expander reparenting, which drops native Gtk focus from
  // focused Bauhaus controls after committed key/scroll edits.

  dt_accels_new_darkroom_action(_modulegroups_switch_tab_next, darktable.develop, N_("Darkroom/Actions"),
                                N_("move to the next modules tab"), GDK_KEY_Tab, GDK_CONTROL_MASK, _("Triggers the action"));
  dt_accels_new_darkroom_action(_modulegroups_switch_tab_previous, darktable.develop, N_("Darkroom/Actions"),
                                N_("move to the previous modules tab"), GDK_KEY_Tab,
                                GDK_CONTROL_MASK | GDK_SHIFT_MASK, _("Triggers the action"));

  dt_accels_new_darkroom_locked_action(_focus_next_module, NULL, N_("Darkroom/Actions"),
                                       N_("Focus on the next module"), GDK_KEY_Page_Down, 0, _("Triggers the action"));
  dt_accels_new_darkroom_locked_action(_focus_previous_module, NULL, N_("Darkroom/Actions"),
                                       N_("Focus on the previous module"), GDK_KEY_Page_Up, 0, _("Triggers the action"));

  dt_accels_new_darkroom_locked_action(_focus_next_control, NULL, N_("Darkroom/Actions"),
                                       N_("Focus on the next module control"), GDK_KEY_Down, GDK_CONTROL_MASK, _("Triggers the action"));
  dt_accels_new_darkroom_locked_action(_focus_previous_control, NULL, N_("Darkroom/Actions"),
                                       N_("Focus on the previous module control"), GDK_KEY_Up, GDK_CONTROL_MASK, _("Triggers the action"));
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_modulegroups_signal_set), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_modulegroups_module_moved), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_modulegroups_refresh), self);

  if(self->data)
  {
    dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
    self->data = NULL;
    if(g_modulegroups_module == self)
    {
      g_modulegroups_module = NULL;
      g_modulegroups_data = NULL;
    }
    dt_conf_set_int("plugins/darkroom/groups", (int)d->current);
    _modulegroups_clear_drop_state(d);
    GtkBox *root = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
    if(darktable.develop && root)
    {
      /* Hand module expanders back to the right-panel root before destroying
       * our page boxes, otherwise Gtk would destroy the module widgets along
       * with the page containers. */
      for(GList *modules = g_list_first(darktable.develop->iop); modules; modules = g_list_next(modules))
      {
        dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
        if(module->expander) _modulegroups_move_widget(module->expander, GTK_WIDGET(root));
      }
    }
    if(d->page_pipeline) gtk_widget_destroy(d->page_pipeline);
    if(d->page_basic) gtk_widget_destroy(d->page_basic);
    if(d->page_repair) gtk_widget_destroy(d->page_repair);
    if(d->page_sharpness) gtk_widget_destroy(d->page_sharpness);
    if(d->page_effects) gtk_widget_destroy(d->page_effects);
    if(d->page_technical) gtk_widget_destroy(d->page_technical);
    if(d->page_all) gtk_widget_destroy(d->page_all);
    d->page_pipeline = NULL;
    d->page_basic = NULL;
    d->page_repair = NULL;
    d->page_sharpness = NULL;
    d->page_effects = NULL;
    d->page_technical = NULL;
    d->page_all = NULL;
    d->section_color = NULL;
    d->section_film = NULL;
    d->section_tones = NULL;
    d->container_color = NULL;
    d->container_film = NULL;
    d->container_tones = NULL;
    dt_free(d);
  }
}

static gboolean _is_module_in_history(const dt_iop_module_t *module)
{
  for(GList *history = g_list_last(darktable.develop->history); history; history = g_list_previous(history))
  {
    const dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)(history->data);
    if(hitem->module == module) return TRUE;
  }

  return FALSE;
}

static gboolean _modulegroups_module_visible_in_current(const dt_lib_modulegroups_t *d, const dt_iop_module_t *module)
{
  if(!d || !module) return FALSE;

  switch(d->current)
  {
    case DT_MODULEGROUP_ACTIVE_PIPE:
      return _is_module_in_history(module) || module->enabled;

    case DT_MODULEGROUP_NONE:
      return !(module->flags() & IOP_FLAGS_DEPRECATED) || module->enabled;

    case DT_MODULEGROUP_TONES:
      return dt_is_module_in_group((dt_iop_module_t *)module, DT_MODULEGROUP_TONES)
             && !(module->flags() & IOP_FLAGS_DEPRECATED);

    default:
      return (d->current == module->default_group()) && !(module->flags() & IOP_FLAGS_DEPRECATED);
  }
}


static void _lib_modulegroups_update_iop_visibility(dt_lib_module_t *self)
{
  if(!self || !self->data || !darktable.develop) return;
  if(darktable.develop->image_storage.id <= 0) return;
  _modulegroups_ensure_page_widgets(self);
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  if(!d->page_pipeline || !d->page_basic || !d->page_repair || !d->page_sharpness
     || !d->page_effects || !d->page_technical || !d->page_all) return;
  _modulegroups_sync_section_label_margins(d);
  _modulegroups_hide_pages(d);

  /* Walk every develop module and decide whether it belongs to the active tab and which box should host it. */
  for(GList *modules = g_list_first(darktable.develop->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    GtkWidget *w = module->expander;

    /* skip modules without a gui */
    if(dt_iop_is_hidden(module)) continue;

    const gboolean visible = _modulegroups_module_visible_in_current(d, module);
    GtkWidget *target = visible ? _modulegroups_target_container(d, module) : NULL;

    if(visible && GTK_IS_WIDGET(w) && GTK_IS_WIDGET(target))
    {
      _modulegroups_setup_drag_source(self, module);
      _modulegroups_move_widget(w, target);
      gtk_widget_show(w);
    }
    else if(GTK_IS_WIDGET(w))
    {
      if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
      gtk_widget_hide(w);
    }
  }

  /* Multishow may hide extra instances, so we only compute section occupancy
   * and final ordering after it has settled the visible module set. */
  dt_dev_modules_update_multishow(darktable.develop);

  if(d->current == DT_MODULEGROUP_ACTIVE_PIPE)
  {
    _modulegroups_reorder_target(d->page_pipeline);
    gtk_widget_show(d->page_pipeline);
  }
  else if(d->current == DT_MODULEGROUP_NONE)
  {
    _modulegroups_reorder_target(d->page_all);
    gtk_widget_show(d->page_all);
  }
  else if(d->current == DT_MODULEGROUP_TONES)
  {
    const gboolean has_color = _modulegroups_reorder_target(d->container_color);
    const gboolean has_film = _modulegroups_reorder_target(d->container_film);
    const gboolean has_tones = _modulegroups_reorder_target(d->container_tones);
    gtk_widget_set_visible(d->section_color, has_color);
    gtk_widget_set_visible(d->container_color, has_color);
    gtk_widget_set_visible(d->section_film, has_film);
    gtk_widget_set_visible(d->container_film, has_film);
    gtk_widget_set_visible(d->section_tones, has_tones);
    gtk_widget_set_visible(d->container_tones, has_tones);
    gtk_widget_show(d->page_basic);
  }
  else
  {
    GtkWidget *target = NULL;
    if(d->current == DT_MODULEGROUP_REPAIR) target = d->page_repair;
    else if(d->current == DT_MODULEGROUP_SHARPNESS) target = d->page_sharpness;
    else if(d->current == DT_MODULEGROUP_EFFECTS) target = d->page_effects;
    else if(d->current == DT_MODULEGROUP_TECHNICAL) target = d->page_technical;

    _modulegroups_reorder_target(target);
    if(target) gtk_widget_show(target);
  }
}

static void _lib_modulegroups_toggle(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(!self || !self->data) return;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  const uint32_t group = _modulegroups_group_from_page(page_num);

  if(d->current == group)
    return; // nothing to do
  else
    d->current = group;

  /* update visibility */
  _lib_modulegroups_update_iop_visibility(self);
}

static void _lib_modulegroups_signal_set(gpointer instance, gpointer module, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(!self || !self->data) return;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  dt_iop_module_t *iop_module = (dt_iop_module_t *)module;

  if(iop_module && !_modulegroups_module_visible_in_current(d, iop_module) && GTK_IS_NOTEBOOK(d->notebook))
  {
    uint32_t group = iop_module->default_group();
    if(group == DT_MODULEGROUP_FILM || group == DT_MODULEGROUP_COLOR) group = DT_MODULEGROUP_TONES;
    if(group < DT_MODULEGROUP_SIZE)
    {
      d->current = group;
      gtk_notebook_set_current_page(GTK_NOTEBOOK(d->notebook), _modulegroups_page_from_group(group));
    }
  }

  _lib_modulegroups_update_iop_visibility(self);
}

static void _lib_modulegroups_module_moved(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(!self || !self->data) return;
  _lib_modulegroups_update_iop_visibility(self);
}

static void _lib_modulegroups_refresh(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(!self || !self->data) return;
  _lib_modulegroups_update_iop_visibility(self);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
