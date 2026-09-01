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

/* The left panel's home for the focused module's blending controls.
 *
 * It owns no blending state and builds no widget of its own beyond a container and a message
 * label: dt_iop_gui_init_blending_body() fills self->widget with the focused module's own
 * blending body, and this module's whole job is to decide which module that is, to tear the
 * previous one down before the next is built, and to say why the panel is empty when no module
 * can host one.
 *
 * The shape manager (libs/shape_manager.c) is a peer, not a parent: it lists the drawn shapes of
 * the whole image in its own window, while the Drawn tab inside the body below speaks only for
 * the focused module. Both rest on develop/masks/ for the shapes themselves.
 */

#include "common/module_versioning.h"  // DT_MODULE()
#include "develop/blend_gui.h"    // dt_iop_gui_{init,cleanup}_blending_body(), dt_iop_gui_update_blending()
#include "develop/develop.h"      // dt_dev_get_global(), dt_history_item_get_name()
#include "develop/imageop.h"      // dt_iop_module_t, IOP_FLAGS_SUPPORTS_BLENDING
#include "develop/imageop_gui.h"  // dt_iop_module_gui_t, for module->gui->blend_data
#include "control/signal.h"       // DT_SIGNAL_DEVELOP_MASKS_GUI_CHANGED
#include "gui/window_manager.h"   // DT_UI_CONTAINER_PANEL_LEFT_CENTER
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "system/macros.h"        // IS_NULL_PTR()
#include "system/mem_alloc.h"     // dt_free()
#include "widgets/widget_settings.h"   // DT_GUI_BOX_SPACING, DT_PIXEL_APPLY_DPI()

#include <glib.h>
#include <gtk/gtk.h>

DT_MODULE(1)

typedef struct dt_lib_blending_t
{
  /* The module whose body is currently built into self->widget, and the one the last refresh
   * ran for. They differ while a refresh is in flight, which is what tells it to tear the
   * previous body down before building the next. */
  dt_iop_module_t *active_module;
  dt_iop_module_t *hosted_module;
} dt_lib_blending_t;


/* The name the panel section has always carried. The module hosts the whole blending body, whose
 * Drawn / Parametric / Raster tabs are the three kinds of mask, so "masking" belongs in it -- and
 * keeping the string spares every translation that already has it. */
const char *name(struct dt_lib_module_t *self __attribute__((unused)))
{
  return _("Masking & Blending");
}

const char **views(dt_lib_module_t *self __attribute__((unused)))
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self __attribute__((unused)))
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int expandable(dt_lib_module_t *self __attribute__((unused)))
{
  return 1;
}

int position()
{
  return 850;
}


/* A module still in dev->iop. The focused module can be one this panel already tore down, or one
 * from a dev that has since been replaced, and neither may be dereferenced. */
static gboolean _module_is_current(const dt_iop_module_t *module)
{
  return dt_dev_get_global() && module && g_list_find(dt_dev_get_global()->iop, module);
}

static void _clear_box(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self) || !GTK_IS_WIDGET(self->widget)) return;

  GList *children = gtk_container_get_children(GTK_CONTAINER(self->widget));
  for(GList *iter = children; iter; iter = g_list_next(iter))
    gtk_widget_destroy(GTK_WIDGET(iter->data));
  g_list_free(children);
  children = NULL;
}

static gboolean _can_host(const dt_iop_module_t *module)
{
  if(!_module_is_current(module) || !module->flags
     || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || !module->gui || !module->gui->blend_data)
    return FALSE;

  return TRUE;
}

static void _release(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->data)) return;
  dt_lib_blending_t *d = (dt_lib_blending_t *)self->data;
  dt_iop_module_t *hosted_module = d->hosted_module;
  d->hosted_module = NULL;

  if(_can_host(hosted_module))
    dt_iop_gui_cleanup_blending_body(hosted_module);
  else
    _clear_box(self);
}

static void _show_message(dt_lib_module_t *self, gchar *markup)
{
  if(IS_NULL_PTR(self) || !GTK_IS_WIDGET(self->widget) || IS_NULL_PTR(markup)) return;

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_widget_set_margin_top(label, DT_PIXEL_APPLY_DPI(16));
  gtk_widget_set_margin_bottom(label, DT_PIXEL_APPLY_DPI(16));
  gtk_widget_set_sensitive(label, FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);
  // self->widget is the expander body: its own visibility encodes the
  // expanded/collapsed state persisted in conf, so show only the child.
  gtk_widget_show_all(label);
}

static void _gui_changed_callback(gpointer instance __attribute__((unused)), dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->data)) return;

  dt_lib_blending_t *d = (dt_lib_blending_t *)self->data;

  /* dt_dev_get_global() is NULL outside the darkroom -- only the views that own that lifetime
   * publish it -- so it is read once and tested before anything is read through it. Nothing
   * reaches this callback that early today (dt_lib_init() runs after dt_view_manager_init(),
   * which darktable.c refuses to continue without a develop), but the guard was written for a
   * NULL it then dereferenced one line above it. */
  dt_develop_t *dev = dt_dev_get_global();
  dt_iop_module_t *module = IS_NULL_PTR(dev) ? NULL : dev->gui_module;

  if(IS_NULL_PTR(dev) || !dev->history || !module)
  {
    _release(self);
    _clear_box(self);

    gchar *markup = g_markup_printf_escaped(_("<i>Select a module to edit its blending settings.</i>"));

    _show_message(self, markup);
    g_free(markup);

    return;
  }

  if(!_can_host(module))
  {
    _release(self);
    _clear_box(self);

    gchar *module_label = dt_history_item_get_name(module);
    gchar *markup = g_markup_printf_escaped(_("<i>Blending is not available for the <b>%s</b> module.</i>"), module_label);

    _show_message(self, markup);
    g_free(markup);
    dt_free(module_label);

    return;
  }

  const gboolean module_changed = (d->active_module != module);
  d->active_module = module;

  if(module_changed) _release(self);

  if(!d->hosted_module) _clear_box(self);

  // dt_iop_gui_init_blending_body() shows the children it packs; don't show
  // self->widget itself, it is the expander body whose visibility encodes the
  // expanded/collapsed state persisted in conf.
  dt_iop_gui_init_blending_body(self->widget, module);
  d->hosted_module = module;

  dt_iop_gui_update_blending(module);
}


void gui_init(dt_lib_module_t *self)
{
  dt_lib_blending_t *d = (dt_lib_blending_t *)dt_calloc_align(sizeof(dt_lib_blending_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_MASKS_GUI_CHANGED,
                                  G_CALLBACK(_gui_changed_callback), self);

  // Show the "select a module" message rather than an empty panel on the first display.
  _gui_changed_callback(NULL, self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_gui_changed_callback), self);

  if(!IS_NULL_PTR(self->data))
  {
    _release(self);
    dt_free(self->data);
    self->data = NULL;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
