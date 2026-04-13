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
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

typedef enum dt_modulesgroups_tabs_t
{
  MOD_TAB_ACTIVE = 0,
  MOD_TAB_BASIC = 1,
  MOD_TAB_REPAIR = 2,
  MOD_TAB_SHARPNESS = 3,
  MOD_TAB_EFFECTS = 4,
  MOD_TAB_TECHNICAL = 5,
  MOD_TAB_ALL = 6,
  MOD_TAB_LAST
} dt_modulesgroups_tabs_t;

typedef enum dt_modulesgroups_basic_sections_t 
{
  TAB_BASIC_COLOR = 0,
  TAB_BASIC_FILM = 1,
  TAB_BASIC_TONES = 2,
  TAB_BASIC_LAST
} dt_modulesgroups_basic_sections_t;

typedef struct dt_lib_modulegroups_t
{
  GtkWidget *notebook;
  GtkWidget *pages[MOD_TAB_LAST];
  GtkWidget *containers[TAB_BASIC_LAST];
  GtkWidget *sections[TAB_BASIC_LAST];
  GtkWidget *drag_highlight;
  dt_iop_module_t *drag_source;
  gboolean inited;
} dt_lib_modulegroups_t;


static dt_modulesgroups_tabs_t _group_to_tab(dt_iop_group_t group)
{
  switch(group)
  {
    case IOP_GROUP_TONES:
    case IOP_GROUP_COLOR:
    case IOP_GROUP_FILM:
      return MOD_TAB_BASIC;

    case IOP_GROUP_EFFECTS:
      return MOD_TAB_EFFECTS;

    case IOP_GROUP_SHARPNESS:
      return MOD_TAB_SHARPNESS;

    case IOP_GROUP_TECHNICAL:
      return MOD_TAB_TECHNICAL;

    case IOP_GROUP_REPAIR:
      return MOD_TAB_REPAIR;

    case IOP_GROUP_LAST:
    case IOP_GROUP_NONE:
      return MOD_TAB_ALL;
  }
  return MOD_TAB_ACTIVE;
}
typedef enum dt_modulegroups_dnd_target_t
{
  DT_MODULEGROUPS_DND_TARGET_IOP = 0
} dt_modulegroups_dnd_target_t;

static const GtkTargetEntry _modulegroups_target_list[] = {
  { "iop", GTK_TARGET_SAME_APP, DT_MODULEGROUPS_DND_TARGET_IOP }
};
static const guint _modulegroups_n_targets = G_N_ELEMENTS(_modulegroups_target_list);

/* toggle button callback */
static void _switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
/* helper function to update iop module view depending on group */
static gboolean _update_iop_visibility(gpointer user_data);

static void _lib_modulegroups_signal_set(gpointer instance, gpointer module, gpointer user_data);
static void _lib_modulegroups_refresh(gpointer instance, gpointer user_data);

static gboolean _focus_next_module();
static gboolean _focus_previous_module();
static gboolean _focus_next_control();
static gboolean _focus_previous_control();
static gboolean _is_module_in_history(const dt_iop_module_t *module);
static void _modulegroups_setup_drag_source(dt_lib_module_t *self, dt_iop_module_t *module);
static gboolean _modulegroups_reorder_target(GtkWidget *target);

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
  if(IS_NULL_PTR(reference)) return;

  GtkBorder margin = { 0 };
  GtkStyleContext *context = gtk_widget_get_style_context(reference);
  gtk_style_context_get_margin(context, gtk_style_context_get_state(context), &margin);

  for(size_t i = 0; i < TAB_BASIC_LAST; i++)
  {
    gtk_widget_set_margin_start(d->sections[i], margin.left);
    gtk_widget_set_margin_end(d->sections[i], margin.right);
    gtk_widget_set_margin_top(d->sections[i], margin.top);
    gtk_widget_set_margin_bottom(d->sections[i], margin.bottom);
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

  if(IS_NULL_PTR(darktable.develop)) return;

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

static void _modulegroups_track_widget(GtkWidget **slot, GtkWidget *widget)
{
  *slot = widget;
  g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)slot);
}

// Because pages are attached to the view right center container,
// and not the our module widget, they are not stable across the lifetime
// of the module, meaning they can get deleted anytime be the view.
// So we need to recreate them dynamically on demand.
static void _ensure_page_widgets(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  GtkBox *root = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  if(IS_NULL_PTR(root)) return;

  // Prepare tab pages
  for(int i = 0; i < MOD_TAB_LAST; i++)
  {
    if(!IS_NULL_PTR(d->pages[i])) continue;
    _modulegroups_track_widget(&d->pages[i], gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_show(d->pages[i]);
    gtk_drag_dest_set(d->pages[i], 0, _modulegroups_target_list, _modulegroups_n_targets, GDK_ACTION_COPY);
    g_signal_connect(d->pages[i], "drag-data-received", G_CALLBACK(_modulegroups_drag_data_received), self);
    g_signal_connect(d->pages[i], "drag-drop", G_CALLBACK(_modulegroups_drag_drop), self);
    g_signal_connect(d->pages[i], "drag-motion", G_CALLBACK(_modulegroups_drag_motion), self);
    g_signal_connect(d->pages[i], "drag-leave", G_CALLBACK(_modulegroups_drag_leave), self);
    gtk_box_pack_start(root, d->pages[i], FALSE, FALSE, 0);
  }

  // Prepare the basic tab page
  for(int i = 0; i < TAB_BASIC_LAST; i++)
  {
    if(IS_NULL_PTR(d->sections[i]))
    {
      switch(i)
      {
        case TAB_BASIC_COLOR:
          _modulegroups_track_widget(&d->sections[i], dt_ui_section_label_new(_("color")));
          break;
        case TAB_BASIC_FILM:
          _modulegroups_track_widget(&d->sections[i], dt_ui_section_label_new(_("film")));
          break;
        case TAB_BASIC_TONES:
          _modulegroups_track_widget(&d->sections[i], dt_ui_section_label_new(_("tones")));
          break;
      }
      gtk_box_pack_start(GTK_BOX(d->pages[MOD_TAB_BASIC]), d->sections[i], FALSE, FALSE, 0);
      gtk_widget_show(d->sections[i]);
    }
    if(IS_NULL_PTR(d->containers[i]))
    {
      _modulegroups_track_widget(&d->containers[i], gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
      gtk_box_pack_start(GTK_BOX(d->pages[MOD_TAB_BASIC]), d->containers[i], FALSE, FALSE, 0);
      gtk_widget_show(d->containers[i]);
    }
  }
}

dt_modulesgroups_tabs_t _modulegroups_cycle_tabs(int tab)
{
  return CLAMP(tab, 0, MOD_TAB_LAST - 1);
}

static dt_modulesgroups_tabs_t _get_current_tab(dt_lib_module_t *self)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(!IS_NULL_PTR(d) && GTK_IS_NOTEBOOK(d->notebook))
    return gtk_notebook_get_current_page(GTK_NOTEBOOK(d->notebook));

  return MOD_TAB_ACTIVE;
}

static void _set_current_tab(dt_lib_module_t *self, dt_modulesgroups_tabs_t tab)
{
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  if(!IS_NULL_PTR(d) && GTK_IS_NOTEBOOK(d->notebook))
    gtk_notebook_set_current_page(GTK_NOTEBOOK(d->notebook), tab);
}

static void _set_current_tab_from_module_group(dt_lib_module_t *self, dt_iop_group_t group)
{
  const dt_modulesgroups_tabs_t tab = _group_to_tab(group);
  _set_current_tab(self, tab);
}

static gboolean _is_module_in_tab(dt_iop_module_t *module, dt_modulesgroups_tabs_t current_tab)
{
  if(IS_NULL_PTR(module) || dt_iop_is_hidden(module)) return FALSE;

  switch(current_tab)
  {
    case MOD_TAB_ACTIVE:
      return (_is_module_in_history(module) || module->enabled);
    case MOD_TAB_ALL:
      return (_is_module_in_history(module) || module->enabled || !(module->flags() & IOP_FLAGS_DEPRECATED));

    default:
    {
      const dt_iop_group_t group = module->default_group();
      const dt_modulesgroups_tabs_t tab = _group_to_tab(group);
      return (tab == current_tab) && (_is_module_in_history(module) || module->enabled || !(module->flags() & IOP_FLAGS_DEPRECATED));
    }
  }

  return FALSE;
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
static GtkWidget *_get_target_container(dt_lib_module_t *self, const dt_iop_module_t *module)
{
  dt_modulesgroups_tabs_t tab = _get_current_tab(self);
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;

  switch(tab)
  {
    case MOD_TAB_BASIC:
    {
      if(IS_NULL_PTR(module)) return NULL;

      // IOP groups tones, color and film go from 1 to 3,
      // and sections go from 0 to 2, so we simply manage the offset.
      dt_iop_group_t group = module->default_group();
      if(group == IOP_GROUP_TONES)
        return d->containers[TAB_BASIC_TONES];
      if(group == IOP_GROUP_COLOR)
        return d->containers[TAB_BASIC_COLOR];
      if(group == IOP_GROUP_FILM)
        return d->containers[TAB_BASIC_FILM];
      return NULL;
    }

    default:
      return d->pages[tab];
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
  if(IS_NULL_PTR(window)) return;

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
  if(IS_NULL_PTR(module_src)) return FALSE;

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

  if(module_src && module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
      dt_iop_gui_move_module_after(module_src, module_dest, "_modulegroups_drag_data_received");
    else
      dt_iop_gui_move_module_before(module_src, module_dest, "_modulegroups_drag_data_received");
  }

  gtk_drag_finish(dc, TRUE, FALSE, time);
  _modulegroups_clear_drop_state(d);
  d->drag_source = NULL;
}

static void _modulegroups_drag_leave(GtkWidget *widget, GdkDragContext *dc, guint time, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  _modulegroups_clear_drop_state(d);
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


static gboolean _modulegroups_switch_tab_next(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                              GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_iop_module_t *focused = darktable.develop->gui_module;
  if(focused) dt_iop_gui_set_expanded(focused, FALSE, TRUE);

  const dt_modulesgroups_tabs_t current = _get_current_tab(self);
  _set_current_tab(self, _modulegroups_cycle_tabs(current + 1));
  dt_iop_request_focus(NULL);

  return TRUE;
}

static gboolean _modulegroups_switch_tab_previous(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                                  GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_iop_module_t *focused = darktable.develop->gui_module;
  if(focused) dt_iop_gui_set_expanded(focused, FALSE, TRUE);

  const dt_modulesgroups_tabs_t current = _get_current_tab(self);
  _set_current_tab(self, _modulegroups_cycle_tabs(current - 1));
  dt_iop_request_focus(NULL);

  return TRUE;
}

static gboolean _scroll_event(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_x, delta_y;

  // We will accumulate scrolls here
  static int scrolls = 0;
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;

  if(dt_gui_get_scroll_unit_deltas(event, &delta_x, &delta_y))
  {
    const dt_modulesgroups_tabs_t current = _get_current_tab(self);
    dt_modulesgroups_tabs_t future = 0;
    if(delta_x > 0. || delta_y > 0.)
      future = current + 1;
    else if(delta_x < 0. || delta_y < 0.)
      future = current - 1;

    _set_current_tab(self, _modulegroups_cycle_tabs(future));
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

/* WARNING: first/last refer to pipeline nodes order, which is reversed compared to GUI order. */
static dt_iop_module_t *_find_first_visible_module(dt_lib_module_t *self)
{
  const dt_modulesgroups_tabs_t current_tab = _get_current_tab(self);

  for(GList *module = g_list_first(darktable.develop->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)module->data;
    if(_is_module_in_tab(mod, current_tab)) return mod;
  }
  return NULL;
}

static dt_iop_module_t *_find_last_visible_module(dt_lib_module_t *self)
{
  const dt_modulesgroups_tabs_t current_tab = _get_current_tab(self);

  for(GList *module = g_list_last(darktable.develop->iop); module; module = g_list_previous(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)module->data;
    if(_is_module_in_tab(mod, current_tab)) return mod;
  }
  return NULL;
}

/* WARNING: next/previous refer to GUI order, which is reversed pipeline order
* in a "layer over" logic: first pipeline node is at the GUI bottom.
*/
static gboolean _focus_previous_module(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                       GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_iop_module_t *focused = darktable.develop->gui_module;
  fprintf(stdout, "focusing previous module\n");

  dt_modulesgroups_tabs_t tab = _get_current_tab(self);
  if(tab == MOD_TAB_BASIC)
  {
    // Basic tab uses internal sections that don't honour pipeline order
    dt_control_log(_("Keyboard navigation in basic tab is not supported yet"));
    return FALSE;
  }

  if(IS_NULL_PTR(focused))
  {
    _focus_module(_find_first_visible_module(self));
  }
  else
  {
    dt_iop_gui_set_expanded(focused, FALSE, TRUE);
    _focus_module(dt_iop_gui_get_next_visible_module(focused));
  }

  return TRUE;
}

static gboolean _focus_next_module(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                   GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_iop_module_t *focused = darktable.develop->gui_module;
  fprintf(stdout, "focusing next module\n");

  dt_modulesgroups_tabs_t tab = _get_current_tab(self);
  if(tab == MOD_TAB_BASIC)
  {
    // Basic tab uses internal sections that don't honour pipeline order
    dt_control_log(_("Keyboard navigation in basic tab is not supported yet"));
    return FALSE;
  }

  if(IS_NULL_PTR(focused))
  {
    _focus_module(_find_last_visible_module(self));
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
  d->inited = FALSE;

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

  for(int i = 0; i < MOD_TAB_LAST; i++)
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
    gtk_widget_show(page);

    d->pages[i] = NULL;
  }
  gtk_notebook_set_current_page(GTK_NOTEBOOK(d->notebook), dt_conf_get_int("plugins/darkroom/moduletab"));
  gtk_notebook_popup_enable(GTK_NOTEBOOK(d->notebook));
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(d->notebook), TRUE);
  g_signal_connect(G_OBJECT(d->notebook), "switch_page", G_CALLBACK(_switch_page), self);
  g_signal_connect(G_OBJECT(d->notebook), "scroll-event", G_CALLBACK(_scroll_event), self);
  gtk_widget_add_events(GTK_WIDGET(d->notebook), darktable.gui->scroll_mask);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->notebook), TRUE, TRUE, 0);
  gtk_widget_show_all(self->widget);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MODULEGROUPS_SET,
                                  G_CALLBACK(_lib_modulegroups_signal_set), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED,
                                  G_CALLBACK(_lib_modulegroups_refresh), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                                  G_CALLBACK(_lib_modulegroups_refresh), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                                  G_CALLBACK(_lib_modulegroups_refresh), self);
  // History edits already trigger explicit modulegroups visibility updates through
  // DT_SIGNAL_DEVELOP_MODULEGROUPS_SET. Listening to generic history changes here
  // causes unnecessary expander reparenting, which drops native Gtk focus from
  // focused Bauhaus controls after committed key/scroll edits.

  dt_accels_new_darkroom_action(_modulegroups_switch_tab_next, self, N_("Darkroom/Actions"),
                                N_("move to the next modules tab"), GDK_KEY_Tab, GDK_CONTROL_MASK, _("Triggers the action"));
  dt_accels_new_darkroom_action(_modulegroups_switch_tab_previous, self, N_("Darkroom/Actions"),
                                N_("move to the previous modules tab"), GDK_KEY_Tab,
                                GDK_CONTROL_MASK | GDK_SHIFT_MASK, _("Triggers the action"));

  dt_accels_new_darkroom_locked_action(_focus_next_module, self, N_("Darkroom/Actions"),
                                       N_("Focus on the next module"), GDK_KEY_Page_Down, 0, _("Triggers the action"));
  dt_accels_new_darkroom_locked_action(_focus_previous_module, self, N_("Darkroom/Actions"),
                                       N_("Focus on the previous module"), GDK_KEY_Page_Up, 0, _("Triggers the action"));

  dt_accels_new_darkroom_locked_action(_focus_next_control, self, N_("Darkroom/Actions"),
                                       N_("Focus on the next module control"), GDK_KEY_Down, GDK_CONTROL_MASK, _("Triggers the action"));
  dt_accels_new_darkroom_locked_action(_focus_previous_control, self, N_("Darkroom/Actions"),
                                       N_("Focus on the previous module control"), GDK_KEY_Up, GDK_CONTROL_MASK, _("Triggers the action"));
}

static gboolean _modulegroups_move_widget(GtkWidget *widget, GtkWidget *target);

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_modulegroups_signal_set), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_modulegroups_refresh), self);

  if(self->data)
  {
    dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
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
    for(int i = 0; i < MOD_TAB_ALL; i++)
    {
      gtk_widget_destroy(d->pages[i]);
      d->pages[i] = NULL;
    }
    dt_free(d);
    self->data = NULL;
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

static gboolean _modulegroups_move_widget(GtkWidget *widget, GtkWidget *target)
{
  if(!GTK_IS_WIDGET(widget) || !GTK_IS_BOX(target)) return FALSE;

  GtkWidget *parent = gtk_widget_get_parent(widget);
  if(parent == target) return FALSE;

  g_object_ref(widget);
  if(GTK_IS_CONTAINER(parent)) gtk_container_remove(GTK_CONTAINER(parent), widget);
  gtk_box_pack_start(GTK_BOX(target), widget, FALSE, FALSE, 0);
  g_object_unref(widget);
  return TRUE;
}

static gboolean _update_iop_visibility(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(IS_NULL_PTR(darktable.develop)) return G_SOURCE_REMOVE;
  dt_lib_modulegroups_t *d = (dt_lib_modulegroups_t *)self->data;
  const dt_modulesgroups_tabs_t tab = _get_current_tab(self);

  _ensure_page_widgets(self);
  _modulegroups_sync_section_label_margins(d);

  // We need both the target and destination parents to be visible while reparenting  
  gtk_widget_show(d->pages[tab]);

  /* Walk every develop module and decide whether it belongs to the active tab and which box should host it. */
  const int history_end = dt_dev_get_history_end_ext(darktable.develop);

  for(GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(dt_iop_is_hidden(module)) continue; // Hidden modules don't have a widget

    GtkWidget *w = module->expander;
    if(IS_NULL_PTR(w)) continue; // Module GUI may not be inited yet

    const gboolean visible = _is_module_in_tab(module, tab);
    GtkWidget *target = _get_target_container(self, module);

    // Extra module instances that are "added" by history items past the current history end conceptually don't exist yet.
    // They exist as disabled pipeline nodes only because they are referenced by an history entry.
    // They will be brought back to life if user move the history end cursor past their history item.
    // For consistency, we hide them from GUI until then.
    // This doesn't apply to base instances.
    // FIXME: at some point, we will need to embrace the nodal paradigm and use a "create instance"
    // approach, even for the first instance, instead of mixing GUI toolboxes à la Lightroom for the first
    // (base) instance and then nodal approach for the others.
    dt_pthread_rwlock_rdlock(&darktable.develop->history_mutex);
    const gboolean in_history = !IS_NULL_PTR(dt_dev_history_get_last_item_by_module(darktable.develop->history, module, history_end));
    dt_pthread_rwlock_unlock(&darktable.develop->history_mutex);

    if(visible && (in_history || module->multi_priority == 0))
    {
      _modulegroups_setup_drag_source(self, module);
      _modulegroups_move_widget(w, target);
      gtk_widget_show(w);
    }
    else
    {
      if(darktable.develop->gui_module == module) 
      {
        dt_iop_request_focus(NULL);
        dt_iop_gui_set_expanded(module, FALSE, TRUE);
      }

      gtk_widget_hide(w);
    }
  }

  /* Multishow may hide extra instances, so we only compute section occupancy
   * and final ordering after it has settled the visible module set. */
  // FIXME: ditch that
  dt_dev_modules_update_multishow(darktable.develop);

  if(tab == MOD_TAB_BASIC)
  {
    for(int i = 0; i < TAB_BASIC_LAST; i++)
    {
      const gboolean has_section = _modulegroups_reorder_target(d->containers[i]);
      gtk_widget_set_visible(d->sections[i], has_section);
      gtk_widget_set_visible(d->containers[i], has_section);
    }
  }
  else
  {
    _modulegroups_reorder_target(d->pages[tab]);
  }

  // Update notebook pages visibility
  for(int i = 0; i < MOD_TAB_LAST; i++) gtk_widget_set_visible(d->pages[i], i == tab);

  // Ensure the parent get refreshed
  gtk_widget_queue_resize(d->pages[tab]);
  gtk_widget_queue_draw(d->pages[tab]);

  // Ensure the module is visible
  if(darktable.develop->gui_module) darktable.gui->scroll_to[1] = darktable.develop->gui_module->expander;

  return G_SOURCE_REMOVE;
}

static void _switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->data)) return;
  if(_get_current_tab(self) != page_num)
  {
    g_idle_add((GSourceFunc)_update_iop_visibility, self);
    dt_conf_set_int("plugins/darkroom/moduletab", page_num);
  }
}

static void _lib_modulegroups_signal_set(gpointer instance, gpointer module, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_iop_module_t *iop_module = (dt_iop_module_t *)module;
  if(IS_NULL_PTR(iop_module)) return;

  // If module not in current tab: switch tab
  if(!_is_module_in_tab(iop_module, _get_current_tab(self)))
    _set_current_tab_from_module_group(self, iop_module->default_group());

  // If module in current tab but not visible: refresh tab
  // This happens when adding new instances or enabling modules through shortcuts
  else if(!dt_iop_is_visible(module))
    _lib_modulegroups_refresh(instance, user_data);
}

static void _lib_modulegroups_refresh(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  g_idle_add((GSourceFunc)_update_iop_visibility, self);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
