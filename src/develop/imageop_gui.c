/*
    This file is part of darktable,
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020-2022 Pascal Obry.
    Copyright (C) 2022 Aldric Renaudin.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2025 Aurélien PIERRE.
    
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

#include "widgets/container.h"
#include "common/telemetry.h"
#include "common/sentry.h"
#include "widgets/gdkkeys.h"
#include "widgets/popup.h"
#include "gui/application.h"
#include "widgets/accelerators.h"
#include "common/collection.h"
#include "common/hash.h"
#include "common/module.h"
#include "common/module_versioning.h"
#include "common/usermanual_url.h"
#include "control/control.h"
#include "control/signal.h"
#include "database/history_repository.h"
#include "database/preset_repository.h"
#include "develop/blend.h"
#include "develop/blend_gui.h"
#include "develop/gui_throttle.h"
#include "develop/masks_gui.h"
#include "develop/dev_pixelpipe.h"
#include "gui/presets.h"
#include "widgets/expander.h"
#include "widgets/label.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"
#include "history/notify.h"
#include "develop/imageop_gui.h"
#define DT_IOP_HEADER_MENU_OPEN "dt-module-header-menu-open"
#define DT_IOP_HEADER_MENU_DISMISS_CLICK "dt-module-header-menu-dismiss-click"

#define DT_IOP_HEADER_IGNORE_RELEASE "dt-module-header-ignore-release"
static void _gui_set_single_expanded(dt_iop_module_t *module, gboolean expanded);
static gboolean _iop_plugin_header_button_release(GtkWidget *w, GdkEventButton *e, gpointer user_data);
static gboolean _iop_plugin_enable_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                        GdkModifierType modifier, gpointer data);
static gboolean _iop_plugin_focus_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                        GdkModifierType modifier, gpointer data);
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "widgets/resetlabel.h"
#include "common/conf.h"
#include "develop/imageop.h"
#include "widgets/bauhaus.h"
#include "widgets/button.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "common/utility.h"


#ifdef GDK_WINDOWING_QUARTZ
#endif

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "widgets/togglebutton.h"

typedef struct dt_module_param_t
{
  dt_iop_module_t *module;
  void            *param;
} dt_module_param_t;

static void _iop_toggle_callback(GtkWidget *togglebutton, dt_module_param_t *data)
{
  if(dt_gui_widgets_suppressed()) return;

  dt_iop_module_t *self = data->module;
  gboolean *field = (gboolean*)(data->param);

  gboolean previous = *field;
  *field = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(togglebutton));

  if(*field != previous)
  {
    dt_iop_gui_changed(self, togglebutton, &previous);
  }
}

// Add to the module internal list of widgets for incremental browsing
// Note: Bauhaus widgets do it internally upon setting label
static void _add_widget_to_module_list(dt_iop_module_t *self, GtkWidget *widget)
{
  if(!IS_NULL_PTR(self) && !IS_NULL_PTR(widget))
  {
    dt_gui_module_t *mod = (dt_gui_module_t *)self;
    mod->widget_list = g_list_append(mod->widget_list, widget);
  }
}

GtkWidget *dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param)
{
  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_iop_params_t *d = (dt_iop_params_t *)self->default_params;

  size_t param_index = 0;
  gboolean skip_label = FALSE;

  const size_t param_length = strlen(param) + 1;
  char *param_name = g_malloc(param_length);
  char *base_name = g_malloc(param_length);
  if(sscanf(param, "%[^[][%" G_GSIZE_FORMAT "]", base_name, &param_index) == 2)
  {
    sprintf(param_name, "%s[0]", base_name);
    skip_label = TRUE;
  }
  else
  {
    memcpy(param_name, param, param_length);
  }
  dt_free(base_name);

  const dt_introspection_field_t *f = self->so->get_f(param_name);

  GtkWidget *slider = NULL;
  size_t offset = 0;

  if(!IS_NULL_PTR(f))
  {
    if(f->header.type == DT_INTROSPECTION_TYPE_FLOAT)
    {
      const float min = f->Float.Min;
      const float max = f->Float.Max;
      offset = f->header.offset + param_index * sizeof(float);
      const float defval = *(float*)((uint8_t *)d + offset);

      const float top = fminf(max-min, fmaxf(fabsf(min), fabsf(max)));
      const int digits = MAX(2, -floorf(log10f(top/100)+.1));

      slider = dt_bauhaus_slider_new_with_range_and_feedback(dt_bauhaus_get_global(), DT_GUI_MODULE(self), min, max, 0, defval, digits, 1);
    }
    else if(f->header.type == DT_INTROSPECTION_TYPE_INT)
    {
      const int min = f->Int.Min;
      const int max = f->Int.Max;
      offset = f->header.offset + param_index * sizeof(int);
      const int defval = *(int*)((uint8_t *)d + offset);

      slider = dt_bauhaus_slider_new_with_range_and_feedback(dt_bauhaus_get_global(), DT_GUI_MODULE(self), min, max, 1, defval, 0, 1);
    }
    else if(f->header.type == DT_INTROSPECTION_TYPE_USHORT)
    {
      const unsigned short min = f->UShort.Min;
      const unsigned short max = f->UShort.Max;
      offset = f->header.offset + param_index * sizeof(unsigned short);
      const unsigned short defval = *(unsigned short*)((uint8_t *)d + offset);

      slider = dt_bauhaus_slider_new_with_range_and_feedback(dt_bauhaus_get_global(), DT_GUI_MODULE(self), min, max, 1, defval, 0, 1);
    }
    else f = NULL;
  }

  if(!IS_NULL_PTR(f))
  {
    dt_bauhaus_widget_set_field(slider, (uint8_t *)p + offset, f->header.type);

    if(!skip_label)
    {
      if (*f->header.description)
      {
        // we do not want to support a context as it break all translations see #5498
        // dt_bauhaus_widget_set_label(slider, g_dpgettext2(NULL, "introspection description", f->header.description));
        dt_bauhaus_widget_set_label(slider, f->header.description);
      }
      else
      {
        gchar *str = dt_util_str_replace(f->header.field_name, "_", " ");

        dt_bauhaus_widget_set_label(slider, str);

        dt_free(str);
      }
    }
  }
  else
  {
    gchar *str = g_strdup_printf("'%s' is not a float/int/unsigned short/slider parameter", param_name);

    slider = dt_bauhaus_slider_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
    dt_bauhaus_widget_set_label(slider, str);

    dt_free(str);
  }

  if(!self->gui->widget) self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), slider, FALSE, FALSE, 0);

  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(slider);
  w->use_default_callback = TRUE;

  dt_free(param_name);

  return slider;
}

GtkWidget *dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param)
{
  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_introspection_field_t *f = self->so->get_f(param);

  GtkWidget *combobox = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  gchar *str = NULL;

  if (!IS_NULL_PTR(f) && (f->header.type == DT_INTROSPECTION_TYPE_ENUM ||
            f->header.type == DT_INTROSPECTION_TYPE_INT  ||
            f->header.type == DT_INTROSPECTION_TYPE_UINT ||
            f->header.type == DT_INTROSPECTION_TYPE_BOOL ))
  {
    dt_bauhaus_widget_set_field(combobox, (uint8_t *)p + f->header.offset, f->header.type);

    if (*f->header.description)
    {
      // we do not want to support a context as it break all translations see #5498
      // dt_bauhaus_widget_set_label(combobox, g_dpgettext2(NULL, "introspection description", f->header.description));
      dt_bauhaus_widget_set_label(combobox, f->header.description);
    }
    else
    {
      str = dt_util_str_replace(f->header.field_name, "_", " ");

      dt_bauhaus_widget_set_label(combobox, str);

      dt_free(str);
    }

    if(f->header.type == DT_INTROSPECTION_TYPE_BOOL)
    {
      dt_bauhaus_combobox_add(combobox, _("no"));
      dt_bauhaus_combobox_add(combobox, _("yes"));
    }
    else if(f->header.type == DT_INTROSPECTION_TYPE_ENUM)
    {
      for(dt_introspection_type_enum_tuple_t *iter = f->Enum.values; iter && iter->name; iter++)
      {
        // we do not want to support a context as it break all translations see #5498
        // dt_bauhaus_combobox_add_full(combobox, g_dpgettext2(NULL, "introspection description", iter->description), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(iter->value), NULL, TRUE);
        if(*iter->description)
          dt_bauhaus_combobox_add_full(combobox, gettext(iter->description), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(iter->value), NULL, TRUE);
      }
    }
  }
  else
  {
    str = g_strdup_printf("'%s' is not an enum/int/bool/combobox parameter", param);

    dt_bauhaus_widget_set_label(combobox, str);

    dt_free(str);
  }

  if(!self->gui->widget) self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), combobox, FALSE, FALSE, 0);

  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(combobox);
  w->use_default_callback = TRUE;

  return combobox;
}

GtkWidget *dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param)
{
  dt_iop_params_t *p = (dt_iop_params_t *)self->params;
  dt_introspection_field_t *f = self->so->get_f(param);

  GtkWidget *button = NULL;
  gchar *str = NULL;

  if(!IS_NULL_PTR(f) && f->header.type == DT_INTROSPECTION_TYPE_BOOL)
  {
    // we do not want to support a context as it break all translations see #5498
    // button = gtk_check_button_new_with_label(g_dpgettext2(NULL, "introspection description", f->header.description));
    str = *f->header.description
        ? g_strdup(f->header.description)
        : dt_util_str_replace(f->header.field_name, "_", " ");

    GtkWidget *label = gtk_label_new(_(str));
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    button = gtk_check_button_new();
    gtk_container_add(GTK_CONTAINER(button), label);
    dt_module_param_t *module_param = (dt_module_param_t *)g_malloc(sizeof(dt_module_param_t));
    module_param->module = self;
    module_param->param = (uint8_t *)p + f->header.offset;
    g_signal_connect_data(G_OBJECT(button), "toggled", G_CALLBACK(_iop_toggle_callback), module_param, (GClosureNotify)g_free, 0);
  }
  else
  {
    str = g_strdup_printf("'%s' is not a bool/togglebutton parameter", param);

    button = gtk_check_button_new_with_label(str);
  }

  _add_widget_to_module_list(self, button);

  dt_free(str);
  if(!self->gui->widget) self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), button, FALSE, FALSE, 0);

  return button;
}

GtkWidget *dt_iop_togglebutton_new(dt_iop_module_t *self, const char *section, const gchar *label, const gchar *ctrl_label,
                                   GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                                   DTGTKCairoPaintIconFunc paint, GtkWidget *box)
{
  GtkWidget *w = dtgtk_togglebutton_new(paint, 0, NULL);
  g_signal_connect(G_OBJECT(w), "button-press-event", callback, self);

  if(IS_NULL_PTR(ctrl_label))
    gtk_widget_set_tooltip_text(w, _(label));
  else
  {
    gchar *tooltip = g_strdup_printf(_("%s\nctrl+click to %s"), _(label), _(ctrl_label));
    gtk_widget_set_tooltip_text(w, tooltip);
    dt_free(tooltip);
  }

  _add_widget_to_module_list(self, w);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), FALSE);
  if(GTK_IS_BOX(box)) gtk_box_pack_end(GTK_BOX(box), w, FALSE, FALSE, 0);

  return w;
}

GtkWidget *dt_iop_togglebutton_new_no_register(dt_iop_module_t *self, const char *section, const gchar *label,
                                               const gchar *ctrl_label, GCallback callback, gboolean local,
                                               guint accel_key, GdkModifierType mods,
                                               DTGTKCairoPaintIconFunc paint, GtkWidget *box)
{
  GtkWidget *w = dtgtk_togglebutton_new(paint, 0, NULL);
  g_signal_connect(G_OBJECT(w), "button-press-event", callback, self);

  if(IS_NULL_PTR(ctrl_label))
    gtk_widget_set_tooltip_text(w, _(label));
  else
  {
    gchar *tooltip = g_strdup_printf(_("%s\nctrl+click to %s"), _(label), _(ctrl_label));
    gtk_widget_set_tooltip_text(w, tooltip);
    dt_free(tooltip);
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), FALSE);
  if(GTK_IS_BOX(box)) gtk_box_pack_end(GTK_BOX(box), w, FALSE, FALSE, 0);

  return w;
}

GtkWidget *dt_iop_button_new(dt_iop_module_t *self, const gchar *label,
                             GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                             DTGTKCairoPaintIconFunc paint, gint paintflags, GtkWidget *box)
{
  GtkWidget *button = NULL;

  if(paint)
  {
    button = dtgtk_button_new(paint, paintflags, NULL);
    gtk_widget_set_tooltip_text(button, _(label));
  }
  else
  {
    button = gtk_button_new_with_label(_(label));
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
  }
  _add_widget_to_module_list(self, button);

  g_signal_connect(G_OBJECT(button), "clicked", callback, (gpointer)self);

  if(GTK_IS_BOX(box)) gtk_box_pack_start(GTK_BOX(box), button, TRUE, TRUE, 0);

  return button;
}

gboolean dt_mask_scroll_increases(int up)
{
  const gboolean mask_down = dt_conf_get_bool("masks_scroll_down_increases");
  return up ? !mask_down : mask_down;
}

/* ------------------------------------------------------------------------------------
 * Everything below was moved verbatim from imageop.c: the module GUI -- header and
 * expander construction, enable button, focus, expansion, rename/duplicate flows,
 * tooltips, mask indicator, the bauhaus bridge and gui_init/update/cleanup. The module
 * lifecycle, params/history core and pipeline defaults stayed behind.
 * ------------------------------------------------------------------------------------ */


static void _iop_color_picker_data_ready_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *const module = user_data;
  GtkWidget *picker = NULL;
  dt_dev_pixelpipe_t *pipe = NULL;
  const dt_dev_pixelpipe_iop_t *piece = NULL;
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->color_picker_apply)) return;
  if(dt_iop_color_picker_get_ready_data(module, &picker, &pipe, &piece)) return;

  dt_print(DT_DEBUG_DEV, "[picker] dispatch module=%s picker=%p pipe=%p hash=%" PRIu64 "\n",
           module->op, (void *)picker, (void *)pipe, piece ? piece->global_hash : 0);

  if(!module->gui->blend_data || !blend_color_picker_apply(module, picker, pipe, (dt_dev_pixelpipe_iop_t *)piece))
    module->color_picker_apply(module, picker, pipe, (dt_dev_pixelpipe_iop_t *)piece);
}

static void _gui_delete_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_develop_t *dev = module->dev;

  // we search another module with the same base
  // we want the next module if any or the previous one
  GList *modules = module->dev->iop;
  dt_iop_module_t *next = NULL;
  int find = 0;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      find = 1;
      if(next) break;
    }
    else if(mod->instance == module->instance)
    {
      next = mod;
      if(find) break;
    }
    modules = g_list_next(modules);
  }
  if(IS_NULL_PTR(next)) return; // what happened ???

  if(module->module_will_remove && !module->module_will_remove(module)) return;

  if(dev->gui_attached) dt_dev_undo_start_record(dev);

  // we must pay attention if priority is 0
  const gboolean is_zero = (module->multi_priority == 0);

  // We are about to destroy this module GUI. Drop darkroom focus first so the
  // center expose callback cannot call module->gui_post_expose() with a stale
  // module->gui_data pointer during teardown-triggered redraws.
  if(dev->gui_module == module) dt_iop_request_focus(NULL);

  dt_gui_freeze_begin();

  // we remove the plugin effectively
  if(!dt_iop_is_hidden(module))
  {
    // we just hide the module to avoid lots of gtk critical warnings
    gtk_widget_hide(module->gui->expander);

    dt_iop_gui_cleanup_module(module);
    dt_gui_refocus_center();
  }

  // we remove all references in the history stack and dev->iop
  // this will inform that a module has been removed from history
  // we do it here so we have the multi_priorities to reconstruct
  // de deleted module if the user undo it
  dt_dev_module_remove(dev, module);

  // if module was priority 0, then we set next to priority 0
  if(is_zero)
  {
    // we want the first one in history
    dt_iop_module_t *first = NULL;
    GList *history = dev->history;
    while(history)
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      if(!hist || !hist->module)
      {
        history = g_list_next(history);
        continue;
      }
      if(hist->module->instance == module->instance && hist->module != module)
      {
        first = hist->module;
        break;
      }
      history = g_list_next(history);
    }
    if(IS_NULL_PTR(first)) first = next;

    // we set priority of first to 0
    dt_iop_update_multi_priority(first, 0);

    // we change this in the history stack too
    for(history = dev->history; history; history = g_list_next(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      // the loop above guards NULL entries in this same list; this one must too
      if(hist && hist->module == first) hist->multi_priority = 0;
    }
  }

  // Commit undo snapshot for the whole delete operation (module removal + multi_priority adjustments).
  if(dev->gui_attached) dt_dev_undo_end_record(dev);

  // Save history
  dt_dev_write_history(dev, FALSE);

  // don't delete the module, a pipe may still need it
  dev->alliop = g_list_append(dev->alliop, module);

  /* redraw */
  dt_dev_pixelpipe_rebuild_all(dev);
  dt_control_queue_redraw_center();

  dt_gui_freeze_end();
}

gboolean dt_iop_gui_module_is_visible(dt_iop_module_t *module)
{
  // callers walk the full dev->iop list: hidden modules (gamma, basebuffer, ...) never get
  // gui_init() and keep module->gui == NULL -- they are simply not visible
  GtkWidget *expander = module->gui ? module->gui->expander : NULL;
  return (expander && gtk_widget_is_visible(expander) && !dt_iop_is_hidden(module));
}

dt_iop_module_t *dt_iop_gui_get_previous_visible_module(dt_iop_module_t *module)
{
  dt_iop_module_t *prev = NULL;
  for(GList *modules = g_list_first(module->dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      break;
    else if(dt_iop_gui_module_is_visible(mod))
      prev = mod;
  }
  return prev;
}

dt_iop_module_t *dt_iop_gui_get_next_visible_module(dt_iop_module_t *module)
{
  dt_iop_module_t *next = NULL;
  for(GList *modules = g_list_last(module->dev->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      break;
    else if(dt_iop_gui_module_is_visible(mod))
      next = mod;
  }
  return next;
}

dt_iop_module_t *dt_iop_gui_duplicate(dt_iop_module_t *base, gboolean copy_params)
{
  // make sure the duplicated module appears in the history
  dt_dev_add_history_item(base->dev, base, FALSE, FALSE);

  // first we create the new module
  dt_gui_freeze_begin();
  dt_iop_module_t *module = dt_dev_module_duplicate(base->dev, base);
  dt_gui_freeze_end();
  if(IS_NULL_PTR(module)) return NULL;

  // we set the gui part of it
  /* initialize gui if iop have one defined */
  if(!dt_iop_is_hidden(module))
  {
    // make sure gui_init and reload defaults is called safely
    dt_iop_gui_init(module);

    /* add module to right panel */
    dt_iop_gui_set_expander(module);
    dt_gui_get_global()->scroll_to_header_once = module->gui->expander;

    dt_iop_reload_defaults(module); // some modules like profiled denoise update the gui in reload_defaults

    if(copy_params)
    {
      memcpy(module->params, base->params, module->params_size);
      if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
      {
        dt_iop_commit_blend_params(module, base->blend_params);
        if(base->blend_params->mask_id > 0)
        {
          module->blend_params->mask_id = 0;
          dt_masks_iop_use_same_as(module, base);
        }
      }
    }

    dt_iop_request_focus(module);
    dt_iop_gui_set_expanded(module, TRUE, FALSE);
    if(base != module && !IS_NULL_PTR(base->gui->expander)) _gui_set_single_expanded(base, FALSE);
    dt_iop_gui_update_blending(module);

    if(module->dev->gui_attached)
      dt_dev_pixelpipe_rebuild_all(module->dev);

    // we save the new instance creation
    dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
  }

  /* update ui to new parameters */
  dt_iop_gui_update(module);

  return module;
}

void dt_iop_gui_rename_module(dt_iop_module_t *module);

/** Rename a freshly-created module after GTK has finished the menu activation.
 *
 * The copy/duplicate actions create and show a new expander while the
 * multi-instance menu is still unwinding its activation path.  Starting the
 * in-place editor immediately can lose the entry focus to the menu teardown,
 * so we wait for the next main-loop idle before installing the entry in the
 * new module header.
 */
static gboolean _rename_module_idle(gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_iop_gui_rename_module(module);

  return G_SOURCE_REMOVE;
}

static void _gui_copy_callback(GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *module = dt_iop_gui_duplicate(user_data, FALSE);
  if(IS_NULL_PTR(module)) return;

  g_idle_add(_rename_module_idle, module);
}

static void _gui_duplicate_callback(GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *module = dt_iop_gui_duplicate(user_data, TRUE);
  if(IS_NULL_PTR(module)) return;

  g_idle_add(_rename_module_idle, module);
}

static gboolean _rename_module_key_press(GtkWidget *entry, GdkEventKey *event, dt_iop_module_t *module)
{
  int ended = 0;
  guint key = dt_keys_mainpad_alternatives(event->keyval);

  if(event->type == GDK_FOCUS_CHANGE || key == GDK_KEY_Return)
  {
    if(gtk_entry_get_text_length(GTK_ENTRY(entry)) > 0)
    {
      // name is not empty, set new multi_name

       const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));

      // restore saved 1st character of instance name (without it the same name wouls still produce unnecessary copy + add history item)
      module->multi_name[0] = module->multi_name[sizeof(module->multi_name) - 1];
      module->multi_name[sizeof(module->multi_name) - 1] = 0;

      if(g_strcmp0(module->multi_name, name) != 0)
      {
        g_strlcpy(module->multi_name, name, sizeof(module->multi_name));
        dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
      }
    }
    else
    {
      // clear out multi-name (set 1st char to 0)
      module->multi_name[0] = 0;
      dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
    }

    ended = 1;
  }
  else if(key == GDK_KEY_Escape)
  {
    // restore saved 1st character of instance name
    module->multi_name[0] = module->multi_name[sizeof(module->multi_name) - 1];
    module->multi_name[sizeof(module->multi_name) - 1] = 0;

    ended = 1;
  }

  if(ended)
  {
    g_signal_handlers_disconnect_by_func(entry, G_CALLBACK(_rename_module_key_press), module);
    gtk_widget_destroy(entry);
    dt_iop_gui_update_header(module);
    dt_masks_group_update_name(module);
    return TRUE;
  }

  return FALSE; /* event not handled */
}

static gboolean _rename_module_resize(GtkWidget *entry, GdkEventKey *event, dt_iop_module_t *module)
{
  int width = 0;
  GtkBorder padding;

  pango_layout_get_pixel_size(gtk_entry_get_layout(GTK_ENTRY(entry)), &width, NULL);
  gtk_style_context_get_padding(gtk_widget_get_style_context (entry),
                                gtk_widget_get_state_flags (entry),
                                &padding);
  gtk_widget_set_size_request(entry, width + padding.left + padding.right + 1, -1);

  return TRUE;
}

void dt_iop_gui_rename_module(dt_iop_module_t *module)
{
  // dt_iop_gui_duplicate() returns NULL when the instance could not be created
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->gui) || IS_NULL_PTR(module->gui->header)) return;
  GtkWidget *focused = gtk_container_get_focus_child(GTK_CONTAINER(module->gui->header));
  if(focused && GTK_IS_ENTRY(focused)) return;

  GtkWidget *entry = gtk_entry_new();
  dt_accels_disconnect_on_text_input(entry);

  gtk_widget_set_name(entry, "iop-panel-label");
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 0);
  gtk_entry_set_max_length(GTK_ENTRY(entry), sizeof(module->multi_name) - 1);
  gtk_entry_set_text(GTK_ENTRY(entry), module->multi_name);

  // remove instance name but save 1st character in case of escape
  module->multi_name[sizeof(module->multi_name) - 1] = module->multi_name[0];
  module->multi_name[0] = 0;
  dt_iop_gui_update_header(module);

  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);
  g_signal_connect(entry, "key-press-event", G_CALLBACK(_rename_module_key_press), module);
  g_signal_connect(entry, "focus-out-event", G_CALLBACK(_rename_module_key_press), module);
  g_signal_connect(entry, "style-updated", G_CALLBACK(_rename_module_resize), module);
  g_signal_connect(entry, "changed", G_CALLBACK(_rename_module_resize), module);

  gtk_box_pack_start(GTK_BOX(module->gui->header), entry, TRUE, TRUE, 0);
  gtk_widget_show(entry);
  gtk_widget_grab_focus(entry);
}

static void _gui_rename_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_iop_gui_rename_module(module);
}

static gboolean _iop_plugin_header_menu_dismiss_idle(gpointer user_data)
{
  GtkWidget *expander = GTK_WIDGET(user_data);
  if(GTK_IS_WIDGET(expander))
    g_object_set_data(G_OBJECT(expander), DT_IOP_HEADER_MENU_DISMISS_CLICK, NULL);

  g_object_unref(expander);
  return G_SOURCE_REMOVE;
}

static void _iop_plugin_header_menu_deactivate(GtkWidget *menu, gpointer user_data)
{
  GtkWidget *expander = GTK_WIDGET(user_data);
  if(!GTK_IS_WIDGET(expander)) return;

  /**
   * Keep the dismiss-click marker until the next main-loop pass. GTK first
   * deactivates the menu, then may deliver the same pointer event to the
   * module header underneath. That event closes the menu only; it must not
   * also toggle the expander state.
   */
  g_object_set_data(G_OBJECT(expander), DT_IOP_HEADER_MENU_OPEN, NULL);
  g_object_set_data(G_OBJECT(expander), DT_IOP_HEADER_MENU_DISMISS_CLICK, GINT_TO_POINTER(TRUE));
  g_idle_add(_iop_plugin_header_menu_dismiss_idle, g_object_ref(expander));
}

static gboolean _gui_multiinstance_callback(GtkButton *button, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if(!IS_NULL_PTR(event) && event->button == 3)
  {
    if(!(module->flags() & IOP_FLAGS_ONE_INSTANCE)) _gui_copy_callback(button, user_data);
    return TRUE;
  }
  else if(!IS_NULL_PTR(event) && event->button == 2)
  {
    return FALSE;
  }

  GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
  GtkWidget *item;

  item = gtk_menu_item_new_with_label(_("new instance"));
  // gtk_widget_set_tooltip_text(item, _("add a new instance of this module to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_copy_callback), module);
  gtk_widget_set_sensitive(item, module->gui->multi_show_new);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("duplicate instance"));
  // gtk_widget_set_tooltip_text(item, _("add a copy of this instance to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_duplicate_callback), module);
  gtk_widget_set_sensitive(item, module->gui->multi_show_new);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("delete"));
  // gtk_widget_set_tooltip_text(item, _("delete this instance"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_delete_callback), module);
  gtk_widget_set_sensitive(item, module->gui->multi_show_close);
  gtk_menu_shell_append(menu, item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  item = gtk_menu_item_new_with_label(_("rename"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_rename_callback), module);
  gtk_menu_shell_append(menu, item);

  if(!IS_NULL_PTR(module->gui->expander))
  {
    g_object_set_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_MENU_OPEN, GINT_TO_POINTER(TRUE));
    g_signal_connect_data(G_OBJECT(menu), "deactivate",
                          G_CALLBACK(_iop_plugin_header_menu_deactivate),
                          g_object_ref(module->gui->expander), (GClosureNotify)g_object_unref, 0);
  }

  dt_gui_menu_popup(GTK_MENU(menu), GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);

  // make sure the button is deactivated now that the menu is opened
  if(button) dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
  return TRUE;
}

static void _gui_off_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if(!dt_gui_widgets_suppressed())
  {
    if(gtk_toggle_button_get_active(togglebutton))
    {
      module->enabled = 1;
      dt_sentry_record_module_usage("iop", module->op);
      dt_telemetry_record_module_usage("iop", module->op);
      dt_dev_add_history_item(module->dev, module, FALSE, TRUE);
    }
    else
    {
      module->enabled = 0;

      //  if current module is set as the CAT instance, remove that setting
      if(module->dev->proxy.chroma_adaptation == module)
        module->dev->proxy.chroma_adaptation = NULL;

      dt_dev_add_history_item(module->dev, module, FALSE, TRUE);
    }
  }

  char tooltip[512];
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(tooltip, sizeof(tooltip), module->enabled ? _("%s is switched on") : _("%s is switched off"),
           module_label);
  dt_free(module_label);
  gtk_widget_set_tooltip_text(GTK_WIDGET(togglebutton), tooltip);
  gtk_widget_queue_draw(GTK_WIDGET(togglebutton));
}

gboolean dt_iop_is_visible(dt_iop_module_t *module)
{
  // module->gui is NULL for hidden modules, and for visible ones between dev->iop
  // construction and gui_init() during darkroom entry
  return !dt_iop_is_hidden(module) && !IS_NULL_PTR(module->gui) && !IS_NULL_PTR(module->gui->expander)
         && gtk_widget_is_visible(module->gui->expander);
}

static void _iop_panel_label(dt_iop_module_t *module)
{
  GtkWidget *lab = dt_gui_container_nth_child(GTK_CONTAINER(module->gui->header), IOP_MODULE_LABEL);
  lab = gtk_bin_get_child(GTK_BIN(lab));
  gtk_widget_set_name(lab, "iop-panel-label");

  char *module_name = dt_history_item_get_label(module);
  dt_capitalize_label(module_name);
  gtk_label_set_markup_with_mnemonic(GTK_LABEL(lab), module_name);
  dt_free(module_name);

  // Module name hasn't changed or no instance name: abort now
  if(!g_strcmp0(module_name, gtk_label_get_text(GTK_LABEL(lab))) || module->multi_name[0] == '\0')
    return;

  dt_gui_module_t *mod = (dt_gui_module_t *)module;
  if(mod->instance_name)
  {
    char *instance_path = dt_accels_build_path(_("Darkroom/Modules/Instances"), mod->instance_name);
    dt_accels_remove_shortcut(dt_gui_get_accels(), instance_path);
    dt_free(instance_path);
    dt_free(mod->instance_name);
  }

  gchar *clean_name = delete_underscore(module->name());
  dt_capitalize_label(clean_name);

  mod->instance_name
      = g_strdup_printf("%s/%s", clean_name, (module->multi_name[0] != '\0') ? module->multi_name : "0");

  dt_accels_new_virtual_instance_shortcut(dt_gui_get_accels(), _iop_plugin_focus_accel, module,
                                          dt_gui_get_accels()->darkroom_accels, _("Darkroom/Modules/Instances"),
                                          mod->instance_name);

  dt_free(clean_name);

  gtk_label_set_ellipsize(GTK_LABEL(lab), !module->multi_name[0] ? PANGO_ELLIPSIZE_END: PANGO_ELLIPSIZE_MIDDLE);
  g_object_set(G_OBJECT(lab), "xalign", 0.0, (gchar *)0);
}

void dt_iop_gui_update_header(dt_iop_module_t *module)
{
  if (IS_NULL_PTR(module->gui)) return;                  /* module has no GUI half at all */
  if (IS_NULL_PTR(module->gui->header))                  /* some modules such as overexposed don't actually have a header */
    return;

  // set panel name to display correct multi-instance
  _iop_panel_label(module);
  dt_iop_gui_set_enable_button(module);
  dt_iop_add_remove_mask_indicator(module);
}

void dt_iop_gui_set_enable_button_icon(GtkWidget *w, dt_iop_module_t *module)
{
  // set on/off icon
  if(module->default_enabled && module->hide_enable_button)
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w), dtgtk_cairo_paint_module_switch_on, 0, module);
  }
  else if(!module->default_enabled && module->hide_enable_button)
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w), dtgtk_cairo_paint_module_switch_on, 0, module);
  }
  else
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w), dtgtk_cairo_paint_module_switch, 0, module);
  }
}

void dt_iop_gui_set_enable_button(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->gui)) return;

  if(module->gui->off)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->gui->off), module->enabled);
    if(module->hide_enable_button)
      gtk_widget_set_sensitive(GTK_WIDGET(module->gui->off), FALSE);
    else
      gtk_widget_set_sensitive(GTK_WIDGET(module->gui->off), TRUE);

    dt_gui_remove_class(GTK_WIDGET(module->gui->off), "dt_iop_enable_forced_on");
    dt_gui_remove_class(GTK_WIDGET(module->gui->off), "dt_iop_enable_forced_off");
    if(module->hide_enable_button)
    {
      dt_gui_add_class(GTK_WIDGET(module->gui->off),
                       module->enabled ? "dt_iop_enable_forced_on" : "dt_iop_enable_forced_off");
    }

    dt_iop_gui_set_enable_button_icon(GTK_WIDGET(module->gui->off), module);
  }
}

void dt_iop_gui_init(dt_iop_module_t *module)
{
  // The module's interactive half exists from here until dt_iop_gui_cleanup_module();
  // its absence IS the headless flag (see dt_iop_module_gui_t in imageop_gui.h).
  if(IS_NULL_PTR(module->gui)) module->gui = (dt_iop_module_gui_t *)calloc(1, sizeof(dt_iop_module_gui_t));

  // Suppress widget value-changed callbacks for the whole GUI build. Setting a slider's soft
  // range (etc.) in gui_init emits "value-changed", which would re-enter the module's
  // gui_changed handler before its sibling widgets exist and crash on dt_bauhaus_*(NULL)
  // (Sentry #129494618, #129578628). The scope guard releases the freeze automatically on every
  // exit path, and the central depth is GUI-thread-only and self-healing, so it cannot drift.
  dt_gui_widget_freeze();

  // Add the accelerators
  if(!dt_iop_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED))
  {
    gchar *clean_name = delete_underscore(module->name());
    dt_capitalize_label(clean_name);

    // slash is not allowed in module names because that makes accel pathes fail
    assert(g_strrstr(clean_name, "/") == NULL);

    dt_gui_module_t *mod = (dt_gui_module_t *)module;
    const gchar *const main_scope = _("Darkroom/Modules");
    mod->accel_path =  dt_accels_build_path(main_scope, clean_name);
    
    dt_accels_new_darkroom_action(_iop_plugin_focus_accel, module, main_scope, clean_name, 0, 0, _("Focuses the module"));

    // NOTE: we should enable the accel only if the module is disable-able, but this property is set at runtime
    // in reload_defaults(), which depends on the image metadata for each pipeline.
    // We have no way of knowing here at init time.
    // if(!module->hide_enable_button)
    dt_accels_new_darkroom_action(_iop_plugin_enable_accel, module, mod->accel_path, _("Enable"), 0, 0, _("Enables the module"));

    dt_free(clean_name);
  }

  // We absolutely need to init the module controls after the module object
  if(module->gui_init) module->gui_init(module);
  if(module->color_picker_apply)
  {
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PICKERDATA_READY,
                                    G_CALLBACK(_iop_color_picker_data_ready_callback), module);
  }
  // the freeze ends here as the scope guard goes out of scope
}

/**
 * @brief Clear GUI pointers that still reference one iop widget being finalized.
 *
 * @param user_data iop module owner.
 * @param where_the_object_was finalized widget address.
 */
static void _iop_gui_widget_gone(gpointer user_data, GObject *where_the_object_was)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(IS_NULL_PTR(module)) return;

  if(!IS_NULL_PTR(module->gui))
  {
    if(module->gui->header == (GtkWidget *)where_the_object_was) module->gui->header = NULL;
    if(module->gui->expander == (GtkWidget *)where_the_object_was) module->gui->expander = NULL;
  }

  if(IS_NULL_PTR(dt_gui_get_global())) return;

  if(dt_gui_get_global()->scroll_to[0] == (GtkWidget *)where_the_object_was) dt_gui_get_global()->scroll_to[0] = NULL;
  if(dt_gui_get_global()->scroll_to[1] == (GtkWidget *)where_the_object_was) dt_gui_get_global()->scroll_to[1] = NULL;
  if(dt_gui_get_global()->scroll_to_header_once == (GtkWidget *)where_the_object_was) dt_gui_get_global()->scroll_to_header_once = NULL;
}

void dt_iop_gui_cleanup_module(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;
  // backend-only modules (hidden, or loaded without gui_init -- see studio-capture.md)
  // have nothing to clean up here
  if(IS_NULL_PTR(module->gui)) return;
  dt_gui_module_t *mod = (dt_gui_module_t *)module;
  if(!IS_NULL_PTR(module->dev) && module->dev->gui_module == module) module->dev->gui_module = NULL;

  // remove multiple delayed gtk_widget_queue_draw triggers
  if(module->gui->widget)
    while(g_idle_remove_by_data(module->gui->widget));

  // Detach accels. accel_path is only ever set by dt_iop_gui_init() (never for a module that was
  // only backend-loaded, e.g. studio_capture's dev->iop -- see studio-capture.md): a NULL path
  // here means there is nothing registered to remove, and dt_accels_remove_accel() would
  // otherwise g_strrstr() every entry in accels->acceleratables against a NULL needle, tripping
  // a GLib-CRITICAL for each one.
  if(!dt_iop_is_hidden(module) && !(module->flags() & IOP_FLAGS_DEPRECATED) && !IS_NULL_PTR(mod->accel_path))
  {
    dt_accels_remove_accel(dt_gui_get_accels(), mod->accel_path, module);
    dt_free(mod->accel_path);
  }

  if(mod->instance_name)
  {
    char *instance_path = dt_accels_build_path(_("Darkroom/Modules/Instances"), mod->instance_name);
    dt_accels_remove_shortcut(dt_gui_get_accels(), instance_path);
    dt_free(instance_path);
  }

  dt_free(mod->instance_name);

  // widget_list doesn't own the widget referenced, so don't deep_free
  dt_gui_module_t *m = DT_GUI_MODULE(module);
  g_list_free(m->widget_list);
  m->widget_list = NULL;
  g_list_free(m->widget_list_bh);
  m->widget_list_bh = NULL;
  dt_free(m->name);
  m->name = NULL;
  dt_free(m->view);
  m->view = NULL;

  if(module->color_picker_apply)
  {
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_iop_color_picker_data_ready_callback), module);
  }
  // History refresh can delete pipeline-only modules created for ordering/history
  // resolution. They have a module GUI cleanup callback but no module GUI data.
  if(module->gui_cleanup && !IS_NULL_PTR(dt_iop_gui_data(module)))
    module->gui_cleanup(module);
  dt_iop_gui_cleanup_blending(module);

  // size-allocate callbacks can still read scroll targets while GTK tears down widgets
  if(!IS_NULL_PTR(dt_gui_get_global()))
  {
    if(dt_gui_get_global()->scroll_to[0] == module->gui->header || dt_gui_get_global()->scroll_to[0] == module->gui->expander)
      dt_gui_get_global()->scroll_to[0] = NULL;
    if(dt_gui_get_global()->scroll_to[1] == module->gui->header || dt_gui_get_global()->scroll_to[1] == module->gui->expander)
      dt_gui_get_global()->scroll_to[1] = NULL;
    if(dt_gui_get_global()->scroll_to_header_once == module->gui->expander)
      dt_gui_get_global()->scroll_to_header_once = NULL;
  }

  /* Release the transient widget tree explicitly. In normal GUI lifetime, these
   * widgets are parented and get destroyed by container teardown. During module
   * probe/init paths, they can stay unparented and would otherwise leak. */
  if(!IS_NULL_PTR(module->gui->expander) && GTK_IS_WIDGET(module->gui->expander))
  {
    GtkWidget *expander = module->gui->expander;
    g_object_ref_sink(expander);
    gtk_widget_destroy(expander);
    g_object_unref(expander);
  }
  else
  {
    if(!IS_NULL_PTR(module->gui->header) && GTK_IS_WIDGET(module->gui->header))
    {
      GtkWidget *header = module->gui->header;
      g_object_ref_sink(header);
      gtk_widget_destroy(header);
      g_object_unref(header);
    }

    if(!IS_NULL_PTR(module->gui->widget) && GTK_IS_WIDGET(module->gui->widget))
    {
      GtkWidget *widget = module->gui->widget;
      g_object_ref_sink(widget);
      gtk_widget_destroy(widget);
      g_object_unref(widget);
    }
  }

  module->gui->widget = NULL;
  module->gui->header = NULL;
  module->gui->expander = NULL;
  module->gui->off = NULL;

  dt_free(module->gui);
  module->gui = NULL;
}

void dt_iop_gui_update(dt_iop_module_t *module)
{
  dt_gui_freeze_begin();
  if(!dt_iop_is_hidden(module))
  {
    if(dt_iop_gui_data(module))
    {
      dt_bauhaus_update_module(module);

      if(module->params && module->gui_update)
        module->gui_update(module);

      dt_iop_gui_update_blending(module);
      dt_iop_gui_update_expanded(module);
    }
    dt_iop_gui_update_header(module);
  }
  dt_gui_freeze_end();
}

static void _gui_reset_callback(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  // never use the callback if module is always disabled
  const gboolean disabled = !module->default_enabled && module->hide_enable_button;
  if(disabled) return;

  //Ctrl is used to apply any auto-presets to the current module
  //If Ctrl was not pressed, or no auto-presets were applied, reset the module parameters
  // FIXME: can we stop with all the easter-eggs key modifiers doing undocumented stuff all along ?
  if(!(event && dt_modifier_is(event->state, GDK_CONTROL_MASK)) || !dt_gui_presets_autoapply_for_module(module))
  {
    /* Resetting a module's parameters does not change GUI focus, so the
       focus-loss cleanup in dt_iop_request_focus() never runs here: an active
       eye-dropper (this module's own, or its blending/masking parametric-mask
       picker -- both share dev->color_picker) and any live mask-shape editing
       state (edit mode, shape buttons, mask/channel display) must be turned
       off explicitly, or they keep sampling/drawing on the image against
       parameters that no longer exist. */
    dt_iop_color_picker_reset(module, FALSE);
    dt_iop_gui_blending_lose_focus(module);

    // if a drawn mask is set, remove it from the list
    if(module->blend_params->mask_id > 0)
    {
      dt_masks_form_t *grp = dt_masks_get_from_id(module->dev, module->blend_params->mask_id);
      // FIXME: ask the user if he wants to delete the mask, or just unlink them.
      if(grp) dt_masks_form_delete(module->dev, module, NULL, grp);
      DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MASK_CHANGED, -1, -1, DT_MASKS_EVENT_RESET);
    }
    /* reset to default params */
    dt_iop_reload_defaults(module);
    dt_iop_commit_blend_params(module, module->default_blendop_params);

    /* reset ui to its defaults */
    dt_iop_gui_reset(module);

    /* update ui to default params*/
    dt_iop_gui_update(module);

    dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
  }
}

static void _presets_popup_callback(GtkButton *button, dt_iop_module_t *module)
{
  const gboolean disabled = !module->default_enabled && module->hide_enable_button;
  if(disabled) return;

  dt_gui_presets_popup_menu_show_for_module(module);

  if(!IS_NULL_PTR(module->gui) && !IS_NULL_PTR(module->gui->expander)
     && !IS_NULL_PTR(dt_gui_get_global()->presets_popup_menu))
  {
    g_object_set_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_MENU_OPEN, GINT_TO_POINTER(TRUE));
    g_signal_connect_data(G_OBJECT(dt_gui_get_global()->presets_popup_menu), "deactivate",
                          G_CALLBACK(_iop_plugin_header_menu_deactivate),
                          g_object_ref(module->gui->expander), (GClosureNotify)g_object_unref, 0);
  }

  dt_gui_menu_popup(dt_gui_get_global()->presets_popup_menu, GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

void dt_iop_request_focus(dt_iop_module_t *module)
{
  dt_develop_t *const dev = dt_dev_get_global();
  dt_iop_module_t *out_focus_module = dev->gui_module;

  if(dt_gui_widgets_suppressed() || (out_focus_module == module)) return;

  dev->gui_module = module;
  if(!IS_NULL_PTR(module) && !IS_NULL_PTR(module->gui))
  {
    const gboolean scroll_new_instance_to_header
      = (dt_gui_get_global()->scroll_to_header_once == module->gui->expander
         && !IS_NULL_PTR(module->gui->header) && GTK_IS_WIDGET(module->gui->header));
    dt_gui_get_global()->scroll_to[1] = scroll_new_instance_to_header ? module->gui->header : module->gui->expander;
  }

  /* lets lose the focus of previous focus module*/
  if(out_focus_module)
  {
    GtkWidget *out_focus_widget = dt_iop_gui_get_pluginui(out_focus_module);
    GtkWidget *scroll_focus = dt_widget_scroll_focus();
    if(scroll_focus && out_focus_widget && gtk_widget_is_ancestor(scroll_focus, out_focus_widget))
    {
      dt_widget_set_scroll_focus(NULL);
      gtk_widget_queue_draw(scroll_focus);
    }

    if(out_focus_module->gui_focus)
      out_focus_module->gui_focus(out_focus_module, FALSE);

    /* A module that loses focus (switching to another module, or collapsing the
       focused module, which calls dt_iop_request_focus(NULL)) must not leave an
       active picker sampling/drawn on the image behind it: the picker's own
       "must have GUI focus" invariant only hides its on-screen overlay, it does
       not stop it or reset the toggle button, so a stale enabled picker can
       resurface unexpectedly (e.g. when the module's GUI is rebuilt). A real
       reset (keep = FALSE) is required here, not the "preserve across a soft
       refresh" keep = TRUE used by gui_update()/gui_changed() call sites. */
    dt_iop_color_picker_reset(out_focus_module, FALSE);
    dt_gui_refocus_center();

    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(out_focus_module), GTK_STATE_FLAG_NORMAL, TRUE);

    /* reset mask view */
    dt_masks_reset_form_gui(out_focus_module->dev);

    /* do stuff needed in the blending gui */
    dt_iop_gui_blending_lose_focus(out_focus_module);

    /* redraw the expander */
    if(!IS_NULL_PTR(out_focus_module->gui)) gtk_widget_queue_draw(out_focus_module->gui->expander);

    /* and finally collection restore hinter messages */
    dt_collection_hint_message(dt_collection_get_global());

    // we also remove the focus css class
    GtkWidget *iop_w = gtk_widget_get_parent(dt_iop_gui_get_pluginui(out_focus_module));
    dt_gui_remove_class(iop_w, "dt_module_focus");
  }

  /* set the focus on module */
  if(!IS_NULL_PTR(module))
  {
    // In case we tried giving focus to a module that is not in the visible tab
    dt_dev_modulegroups_switch_tab(module->dev, module);

    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(module), GTK_STATE_FLAG_SELECTED, TRUE);

    if(module->gui_focus) module->gui_focus(module, TRUE);

    /* redraw the expander */
    if(!IS_NULL_PTR(module->gui))
    {
      gtk_widget_queue_draw(module->gui->expander);
      gtk_widget_grab_focus(module->gui->expander);
    }

    /* set the focus on the first child to enable arrow-key navigation and accessibility stuff */
    GList *widget_list = ((dt_gui_module_t *)module)->widget_list;
    if(widget_list)
    {
      GList *first_child = g_list_first(widget_list);
      if(first_child)
      {
        GtkWidget *widget = (GtkWidget *)first_child->data;
        if(widget) gtk_widget_grab_focus(widget);
      }
    }

    // we also add the focus css class
    GtkWidget *iop_w = gtk_widget_get_parent(dt_iop_gui_get_pluginui(dev->gui_module));
    dt_gui_add_class(iop_w, "dt_module_focus");
  }

  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_MASKS_GUI_CHANGED);
  dt_control_queue_cursor(GDK_LEFT_PTR);
  dt_control_queue_redraw_center();
}

/*
 * NEW EXPANDER
 */

static void _gui_set_single_expanded(dt_iop_module_t *module, gboolean expanded)
{
  // reached for every module in dev->iop via the collapse_others fan-out: gui-less
  // (hidden / not-yet-inited) modules have nothing to collapse
  if(IS_NULL_PTR(module->gui) || IS_NULL_PTR(module->gui->expander)) return;

  /* update expander arrow state */
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->gui->expander), expanded);

  /* store expanded state of module.
   * we do that first, so update_expanded won't think it should be visible
   * and undo our changes right away. */
  module->gui->expanded = expanded;

  /* show / hide plugin widget */
  if(expanded)
  {
    /* set this module to receive focus / draw events*/
    dt_iop_request_focus(module);

    /* focus the current module */
    for(int k = 0; k < DT_UI_CONTAINER_SIZE; k++)
      dt_ui_container_focus_widget(dt_gui_get_ui(), k, module->gui->expander);

    /* redraw center, iop might have post expose */
    dt_control_queue_redraw_center();
  }
  else
  {
    if(module->dev->gui_module == module)
    {
      dt_iop_request_focus(NULL);
      dt_control_queue_redraw_center();
    }
  }

  if(expanded)
    dt_gui_add_class(module->gui->expander, "expanded");
  else
    dt_gui_remove_class(module->gui->expander, "expanded");

  char var[1024];
  snprintf(var, sizeof(var), "plugins/darkroom/%s/expanded", module->op);
  dt_conf_set_bool(var, expanded);
}

/** Dim all modules except the one referenced, if any reference, or undim all */
void _iop_dim_all_but(dt_iop_module_t *module, gboolean dim)
{
  for(GList *iop = g_list_first(dt_dev_get_global()->iop); iop; iop = g_list_next(iop))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iop->data;

    // Handle invisible modules
    if(IS_NULL_PTR(m) || !m->gui || !m->gui->expander) continue;

    if(dim && m != module)
      dt_gui_add_class(gtk_widget_get_parent(dt_iop_gui_get_pluginui(m)), "module-dimmed");
    else
      dt_gui_remove_class(gtk_widget_get_parent(dt_iop_gui_get_pluginui(m)), "module-dimmed");
  }
}

void dt_iop_gui_set_expanded(dt_iop_module_t *module, gboolean expanded, gboolean collapse_others)
{
  if(IS_NULL_PTR(module) || !module->gui || !module->gui->expander) return;
  if(collapse_others)
  {
    for(GList *iop = g_list_first(module->dev->iop); iop; iop = g_list_next(iop))
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
      if(m != module) _gui_set_single_expanded(m, FALSE);
    }
  }

  _gui_set_single_expanded(module, expanded);
  _iop_dim_all_but((expanded) ? module : NULL, expanded);
  gtk_widget_queue_draw(module->gui->widget);
}

void dt_iop_gui_update_expanded(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module->gui->expander)) return;

  const gboolean expanded = module->gui->expanded;

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->gui->expander), expanded);
}

static gboolean _iop_plugin_body_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  /* Reset the scrolling focus. If the click happened on any bauhaus element,
   * its internal button_press method will set it for itself */
  dt_widget_set_scroll_focus(NULL);

  gboolean handled = FALSE;

  if(e->button == 1)
  {
    dt_iop_request_focus(module);
    handled = TRUE;
  }
  else if(e->button == 3)
  {
    _presets_popup_callback(NULL, module);
    handled = TRUE;
  }
  return handled;
}

static gboolean _iop_plugin_header_activate(GtkWidget* self, gboolean group_cycling, gpointer user_data)
{
  dt_gui_module_t *module = (dt_gui_module_t *)user_data;
  if(IS_NULL_PTR(module) || !module->focus) return FALSE;
  return module->focus(module, TRUE);
}

static gboolean _iop_plugin_header_child_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(module && module->gui->expander)
    g_object_set_data(G_OBJECT(module->gui->expander), "dt-module-header-child-click", GINT_TO_POINTER(TRUE));

  return FALSE;
}

static gboolean _iop_plugin_focus_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_gui_module_t *module = (dt_gui_module_t *)data;
  dt_iop_module_t *iop = (dt_iop_module_t *)data;
  if(IS_NULL_PTR(module) || !module->focus) return FALSE;

  // Accel search explicitly targets a module, so allow modulegroups to leave
  // the Pipeline tab once for this focus request.
  if(iop->gui->expander)
    g_object_set_data(G_OBJECT(iop->gui->expander), "dt-modulegroups-switch-from-active-once",
                      GINT_TO_POINTER(TRUE));

  return module->focus(module, FALSE);
}

static gboolean _iop_plugin_enable_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                         GdkModifierType modifier, gpointer data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)data;
  if(IS_NULL_PTR(module)) return FALSE;

  // Direct actions from accel search should prioritize Pipeline when they focus
  // the edited module right after applying the change.
  if(!IS_NULL_PTR(module->gui->expander))
    g_object_set_data(G_OBJECT(module->gui->expander), "dt-modulegroups-prefer-active-once",
                      GINT_TO_POINTER(TRUE));

  // Kind of ugly to go through history to change module GUI state
  // FIXME: we should have a GUI callback that enables module and dispatches history instead, 
  // history should not care about module GUI. This is a wrong, reversed inheritance.
  if(!module->hide_enable_button)
  {
    module->enabled = TRUE;
    dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
  }

  dt_iop_request_focus(module);
  dt_iop_gui_set_expanded(module, TRUE, TRUE);
  return TRUE;
}

static gboolean _iop_plugin_header_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return TRUE;

  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(IS_NULL_PTR(module)) return FALSE;

  if(!IS_NULL_PTR(module->gui->expander)
     && (g_object_get_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_MENU_OPEN)
         || g_object_get_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_MENU_DISMISS_CLICK)))
  {
    g_object_set_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_IGNORE_RELEASE, GINT_TO_POINTER(TRUE));
    return TRUE;
  }

  /* Reset the scrolling focus. If the click happened on any bauhaus element,
   * its internal button_press method will set it for itself */
  dt_widget_set_scroll_focus(NULL);

  if(e->button == 1)
  {
    if(module->gui->expander) g_object_set_data(G_OBJECT(module->gui->expander), "dt-module-dragged", NULL);

    if(!dt_modifier_is(e->state, GDK_CONTROL_MASK))
    {
      return FALSE;
    }
    else
    {
      if(module->gui->expander)
        g_object_set_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_IGNORE_RELEASE, GINT_TO_POINTER(TRUE));
      dt_iop_gui_rename_module(module);
      return TRUE;
    }
  }
  else if(e->button == 3)
  {
    _presets_popup_callback(NULL, module);

    return TRUE;
  }
  return FALSE;
}

static gboolean _iop_plugin_header_button_release(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  if(e->button != 1 || e->type != GDK_BUTTON_RELEASE) return FALSE;

  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(IS_NULL_PTR(module) || !module->gui->expander) return FALSE;

  if(g_object_get_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_IGNORE_RELEASE))
  {
    g_object_set_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_IGNORE_RELEASE, NULL);
    return TRUE;
  }

  if(g_object_get_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_MENU_OPEN)
     || g_object_get_data(G_OBJECT(module->gui->expander), DT_IOP_HEADER_MENU_DISMISS_CLICK))
  {
    return TRUE;
  }

  if(g_object_get_data(G_OBJECT(module->gui->expander), "dt-module-header-child-click"))
  {
    g_object_set_data(G_OBJECT(module->gui->expander), "dt-module-header-child-click", NULL);
    return FALSE;
  }

  if(g_object_get_data(G_OBJECT(module->gui->expander), "dt-module-dragged"))
  {
    g_object_set_data(G_OBJECT(module->gui->expander), "dt-module-dragged", NULL);
    return TRUE;
  }

  // make gtk scroll to the module once it updated its allocation size
  const gboolean collapse_others = dt_modifier_is(e->state, GDK_SHIFT_MASK) ? FALSE : TRUE;
  dt_iop_request_focus(module);
  dt_iop_gui_set_expanded(module, !module->gui->expanded, collapse_others);

  return TRUE;
}

static void _display_mask_indicator_callback(GtkToggleButton *bt, dt_iop_module_t *module)
{
  if(dt_gui_widgets_suppressed()) return;

  const gboolean is_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bt));
  const dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->gui->blend_data;

  module->request_mask_display
      &= ~(DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL
           | DT_DEV_PIXELPIPE_DISPLAY_ANY | DT_DEV_PIXELPIPE_DISPLAY_STICKY);

  if(is_active)
    module->request_mask_display |= DT_DEV_PIXELPIPE_DISPLAY_MASK;

  dt_iop_set_cache_bypass(module, module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);

  // set the module show mask button too
  if(bd->showmask)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), is_active);

  dt_gui_freeze_begin();
  if(GTK_IS_TOGGLE_BUTTON(bd->filter[0].channel_display))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->filter[0].channel_display), FALSE);
  if(GTK_IS_TOGGLE_BUTTON(bd->filter[1].channel_display))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->filter[1].channel_display), FALSE);
  dt_gui_freeze_end();

  dt_iop_request_focus(module);
  dt_dev_pixelpipe_update_history_main(module->dev);
}

static void _mask_indicator_get_usage(dt_iop_module_t *module, gboolean *top_enabled, gboolean *raster_used,
                                      gboolean *drawn_used, gboolean *parametric_used)
{
  if(!IS_NULL_PTR(top_enabled)) *top_enabled = FALSE;
  if(!IS_NULL_PTR(raster_used)) *raster_used = FALSE;
  if(!IS_NULL_PTR(drawn_used)) *drawn_used = FALSE;
  if(!IS_NULL_PTR(parametric_used)) *parametric_used = FALSE;

  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->blend_params)) return;

  const dt_iop_gui_blend_data_t *bd = (const dt_iop_gui_blend_data_t *)module->gui->blend_data;
  gboolean top = FALSE;
  gboolean raster = FALSE;
  gboolean drawn = FALSE;
  gboolean parametric = FALSE;
  dt_develop_blend_get_mask_usage(module, module->blend_params, &top, &raster, &drawn, &parametric);

  // look if the user disabled masks modes

  // raster mask must be enabled and a raster mask must be selected in the combo
  if(!IS_NULL_PTR(bd) && GTK_IS_TOGGLE_BUTTON(bd->raster_enable)
     && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->raster_enable)))
    raster = FALSE;

  if(!IS_NULL_PTR(bd) && GTK_IS_TOGGLE_BUTTON(bd->masks_enable)
     && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->masks_enable)))
    drawn = FALSE;

  if(!IS_NULL_PTR(bd) && GTK_IS_TOGGLE_BUTTON(bd->blendif_enable)
     && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bd->blendif_enable)))
    parametric = FALSE;

  if(!IS_NULL_PTR(top_enabled)) *top_enabled = top;
  if(!IS_NULL_PTR(raster_used)) *raster_used = raster;
  if(!IS_NULL_PTR(drawn_used)) *drawn_used = drawn;
  if(!IS_NULL_PTR(parametric_used)) *parametric_used = parametric;
}

static gboolean _mask_indicator_tooltip(GtkWidget *treeview, gint x, gint y, gboolean kb_mode,
      GtkTooltip* tooltip, dt_iop_module_t *module)
{
  (void)treeview;
  (void)x;
  (void)y;
  (void)kb_mode;

  gboolean res = FALSE;
  if(module->gui->mask_indicator)
  {
    gchar *type = _("unknown mask");
    gchar *text;
    gboolean top_enabled = FALSE, raster_used = FALSE, drawn_used = FALSE, parametric_used = FALSE;
    _mask_indicator_get_usage(module, &top_enabled, &raster_used, &drawn_used, &parametric_used);
    if(!top_enabled) return FALSE;

    if(drawn_used && parametric_used)
      type=_("drawn + parametric mask");
    else if(drawn_used)
      type=_("drawn mask");
    else if(parametric_used)
      type=_("parametric mask");
    else if(raster_used)
      type=_("raster mask");
    else
      return FALSE;
    gchar *part1 = g_strdup_printf(_("this module has a '%s'"), type);
    gchar *part2 = NULL;
    if(raster_used && module->raster_mask.sink.source)
    {
      gchar *source = dt_history_item_get_name(module->raster_mask.sink.source);
      part2 = g_strdup_printf(_("taken from module %s"), source);
      dt_free(source);
    }

    if(part2)
    {
      gchar *details = g_strdup_printf("%s\n%s", part2, _("click to display (module must be activated first)"));
      dt_free(part2);
      part2 = details;
    }
    else
    {
      part2 = g_strdup(_("click to display (module must be activated first)"));
    }

    if(part2)
      text = g_strconcat(part1, "\n", part2, NULL);
    else
      text = g_strdup(part1);

    gtk_tooltip_set_text(tooltip, text);
    res = TRUE;
    dt_free(part1);
    dt_free(part2);
    dt_free(text);
  }
  return res;
}

void dt_iop_add_remove_mask_indicator(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->gui) || IS_NULL_PTR(module->gui->mask_indicator)) return;

  const gboolean support_blending = (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) == IOP_FLAGS_SUPPORTS_BLENDING;

  if(!support_blending || !module->blend_params)
  {
    gtk_widget_set_visible(GTK_WIDGET(module->gui->mask_indicator), FALSE);
    gtk_widget_set_has_tooltip(GTK_WIDGET(module->gui->mask_indicator), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(module->gui->mask_indicator), FALSE);
    return;
  }

  gboolean top_enabled = FALSE, raster_used = FALSE, drawn_used = FALSE, parametric_used = FALSE;
  _mask_indicator_get_usage(module, &top_enabled, &raster_used, &drawn_used, &parametric_used);

  const gboolean use_masks = top_enabled && (raster_used || drawn_used || parametric_used);

  gtk_widget_set_visible(GTK_WIDGET(module->gui->mask_indicator), use_masks);
  gtk_widget_set_sensitive(GTK_WIDGET(module->gui->mask_indicator), module->enabled);
  gtk_widget_set_has_tooltip(GTK_WIDGET(module->gui->mask_indicator), use_masks);
}

gboolean _iop_tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                               GtkTooltip *tooltip, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  const char **des = module->description(module);

  if(IS_NULL_PTR(des)) return FALSE;

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_GUI_BOX_SPACING);
  gtk_widget_set_hexpand(grid, FALSE);

  GtkWidget *label = gtk_label_new(des[0] ? des[0] : "");
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
  // if there is no more description, do not add a separator
  if(des[1]) dt_gui_add_class(label, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

  gtk_widget_set_size_request(label, DT_PIXEL_APPLY_DPI(300), -1);
  gtk_widget_set_size_request(grid, DT_PIXEL_APPLY_DPI(300), -1);
  gtk_widget_set_size_request(vbox, DT_PIXEL_APPLY_DPI(300), -1);

  const char *icon_purpose = "\342\237\263";
  const char *icon_input   = "\342\207\245";
  const char *icon_process = "\342\237\264";
  const char *icon_output  = "\342\206\246";

  const char *icons[4] = {icon_purpose, icon_input, icon_process, icon_output};
  const char *ilabs[4] = {_("Purpose"), _("Input"), _("Process"), _("Output")};

  for(int k=1; k<5; k++)
  {
    if(des[k])
    {
      label = gtk_label_new(icons[k-1]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 0, k, 1, 1);
      gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

      label = gtk_label_new(ilabs[k-1]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 1, k, 1, 1);
      gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

      label = gtk_label_new(":");
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 2, k, 1, 1);
      gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

      label = gtk_label_new(des[k]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 3, k, 1, 1);
      gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    }
  }

  gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);
  gtk_widget_show_all(vbox);
  gtk_tooltip_set_custom(tooltip, vbox);

  return TRUE;
}

void dt_iop_gui_set_expander(dt_iop_module_t *module)
{
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING / 2.);
  gtk_widget_set_name(GTK_WIDGET(header), "module-header");

  GtkWidget *iopw = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  GtkWidget *expander = dtgtk_expander_new(header, iopw);
  dt_gui_add_class(expander, "dt_module_frame");
  dt_gui_add_class(expander, "dt_iop_module");

  GtkWidget *header_evb = dtgtk_expander_get_header_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *body_evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *pluginui_frame = dtgtk_expander_get_frame(DTGTK_EXPANDER(expander));

  dt_gui_add_class(pluginui_frame, "dt_plugin_ui");

  module->gui->header = header;

  /* setup the header box */
  g_signal_connect(G_OBJECT(header_evb), "button-press-event", G_CALLBACK(_iop_plugin_header_button_press), module);
  g_signal_connect(G_OBJECT(header_evb), "button-release-event", G_CALLBACK(_iop_plugin_header_button_release), module);
  g_signal_connect(G_OBJECT(header_evb), "mnemonic-activate", G_CALLBACK(_iop_plugin_header_activate), module);
  gtk_widget_add_events(header_evb, GDK_POINTER_MOTION_MASK);

  /* connect mouse button callbacks for focus and presets */
  g_signal_connect(G_OBJECT(body_evb), "button-press-event", G_CALLBACK(_iop_plugin_body_button_press), module);
  gtk_widget_add_events(body_evb, GDK_POINTER_MOTION_MASK);

  /*
   * initialize the header widgets
   */
  GtkWidget *hw[IOP_MODULE_LAST] = { NULL };

  /* init empty place for icon, this is then set in CSS if needed */
  char w_name[256] = { 0 };
  snprintf(w_name, sizeof(w_name), "iop-panel-icon-%s", module->op);
  hw[IOP_MODULE_ICON] = gtk_label_new("");
  gtk_widget_set_name(GTK_WIDGET(hw[IOP_MODULE_ICON]), w_name);

  /* add module label */
  hw[IOP_MODULE_LABEL] = gtk_event_box_new();
  GtkWidget *lab = hw[IOP_MODULE_LABEL];
  GtkWidget *label = gtk_label_new_with_mnemonic("");
  gtk_container_add(GTK_CONTAINER(lab), label);
  gtk_label_set_mnemonic_widget(GTK_LABEL(label), header_evb);

  if((module->flags() & IOP_FLAGS_DEPRECATED) && module->deprecated_msg())
    gtk_widget_set_tooltip_text(lab, module->deprecated_msg());
  else
  {
    gtk_widget_set_name(lab, "iop_description");
    g_signal_connect(lab, "query-tooltip", G_CALLBACK(_iop_tooltip_callback), module);
  }

  /* add mask preview button */
  hw[IOP_MODULE_MASK] = dtgtk_togglebutton_new(dtgtk_cairo_paint_showmask, 0, NULL);

  g_signal_connect(G_OBJECT(hw[IOP_MODULE_MASK]), "toggled",
                    G_CALLBACK(_display_mask_indicator_callback), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_MASK]), "button-press-event",
                   G_CALLBACK(_iop_plugin_header_child_button_press), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_MASK]), "query-tooltip",
                    G_CALLBACK(_mask_indicator_tooltip), module);
  module->gui->mask_indicator = hw[IOP_MODULE_MASK];

  /* add multi instances menu button */
  hw[IOP_MODULE_INSTANCE] = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, 0, NULL);
  module->gui->multimenu_button = GTK_WIDGET(hw[IOP_MODULE_INSTANCE]);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_INSTANCE]),
                              _("multiple instance actions\nright-click creates new instance"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_INSTANCE]), "button-press-event",
                   G_CALLBACK(_iop_plugin_header_child_button_press), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_INSTANCE]), "button-press-event", G_CALLBACK(_gui_multiinstance_callback),
                   module);

  dt_gui_add_help_link(expander, dt_get_help_url(module->op));

  /* add reset button */
  hw[IOP_MODULE_RESET] = dtgtk_button_new(dtgtk_cairo_paint_reset, 0, NULL);
  module->gui->reset_button = GTK_WIDGET(hw[IOP_MODULE_RESET]);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_RESET]), _("reset parameters\nctrl+click to reapply any automatic presets"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_RESET]), "button-press-event",
                   G_CALLBACK(_iop_plugin_header_child_button_press), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_RESET]), "button-press-event", G_CALLBACK(_gui_reset_callback), module);

  /* add preset button if module has implementation */
  hw[IOP_MODULE_PRESETS] = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
  module->gui->presets_button = GTK_WIDGET(hw[IOP_MODULE_PRESETS]);
  if(!(module->flags() & IOP_FLAGS_ONE_INSTANCE))
    gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_PRESETS]), _("presets\nright-click to apply on new instance"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_PRESETS]), "button-press-event",
                   G_CALLBACK(_iop_plugin_header_child_button_press), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_PRESETS]), "clicked", G_CALLBACK(_presets_popup_callback), module);

  /* add enabled button */
  GtkWidget *switch_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_module_switch, 0, module);

  dt_gui_add_class(switch_button, "dt_iop_enable_button");
  dt_iop_gui_set_enable_button_icon(switch_button, module);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(switch_button), module->enabled);
  g_signal_connect(G_OBJECT(switch_button), "button-press-event",
                   G_CALLBACK(_iop_plugin_header_child_button_press), module);
  g_signal_connect(G_OBJECT(switch_button), "toggled", G_CALLBACK(_gui_off_callback), module);

  module->gui->off = switch_button;
  gtk_widget_set_sensitive(GTK_WIDGET(switch_button), !module->hide_enable_button);

  /* Wrap the switch in a plain box so the CSS spacing trick that visually tucks it
     against the header edge (negative margin, see ansel.css) lives on the wrapper,
     not on the button itself. dtgtk_togglebutton's custom draw() reinterprets its own
     CSS margin a second time (as a paint-only offset) to size its icon -- if that same
     margin also carries a position nudge, the icon gets painted outside the button's
     real GTK allocation, which is what drives hover/click hit-testing. Keeping the
     button's own margin at 0 keeps its drawing and its hit-region intrinsically in
     sync; the wrapper absorbs the nudge at the layout level instead. */
  hw[IOP_MODULE_SWITCH] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(hw[IOP_MODULE_SWITCH], "dt_iop_enable_button_box");
  gtk_container_add(GTK_CONTAINER(hw[IOP_MODULE_SWITCH]), switch_button);

  /* reorder header, for now, iop are always in the right panel */
  for(int i = 0; i <= IOP_MODULE_LABEL; i++)
  {
    if(hw[i]) 
      gtk_box_pack_start(GTK_BOX(header), hw[i], FALSE, FALSE, 0);
  }
  for(int i = IOP_MODULE_LAST - 1; i > IOP_MODULE_LABEL; i--)
  {
    if(hw[i]) 
      gtk_box_pack_end(GTK_BOX(header), hw[i], FALSE, FALSE, 0);
  }

  dt_gui_add_help_link(header, dt_get_help_url("module_header"));
  // for the module label, point to module specific help page
  dt_gui_add_help_link(hw[IOP_MODULE_LABEL], dt_get_help_url(module->op));

  gtk_widget_set_halign(hw[IOP_MODULE_LABEL], GTK_ALIGN_START);
  gtk_widget_set_halign(hw[IOP_MODULE_INSTANCE], GTK_ALIGN_END);

  // show deprecated message if any
  if(module->deprecated_msg())
  {
    GtkWidget *lb = gtk_label_new(module->deprecated_msg());
    gtk_label_set_line_wrap(GTK_LABEL(lb), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
    dt_gui_add_class(lb, "dt_warning");
    gtk_box_pack_start(GTK_BOX(iopw), lb, TRUE, TRUE, 0);
    gtk_widget_show(lb);
  }

  /* initialize blending state if supported; the detached widget is hosted by the masks lib */
  gtk_box_pack_start(GTK_BOX(iopw), module->gui->widget, TRUE, TRUE, 0);
  dt_iop_gui_init_blending(module);
  dt_gui_add_class(module->gui->widget, "dt_plugin_ui_main");
  dt_gui_add_help_link(module->gui->widget, dt_get_help_url(module->op));
  gtk_widget_hide(iopw);

  module->gui->expander = expander;
  g_object_weak_ref(G_OBJECT(header), _iop_gui_widget_gone, module);
  g_object_weak_ref(G_OBJECT(expander), _iop_gui_widget_gone, module);

  /* update header */
  dt_iop_gui_update_header(module);

  gtk_widget_set_hexpand(module->gui->widget, FALSE);
  gtk_widget_set_vexpand(module->gui->widget, FALSE);

  dt_ui_container_add_widget(dt_gui_get_ui(), DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
}

GtkWidget *dt_iop_gui_get_widget(dt_iop_module_t *module)
{
  // NULL for a module with no GUI half, the way the flat module->expander was NULL before
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->gui) || IS_NULL_PTR(module->gui->expander)) return NULL;
  return dtgtk_expander_get_body(DTGTK_EXPANDER(module->gui->expander));
}

GtkWidget *dt_iop_gui_get_pluginui(dt_iop_module_t *module)
{
  // return gtkframe (pluginui_frame)
  if(IS_NULL_PTR(module) || IS_NULL_PTR(module->gui) || IS_NULL_PTR(module->gui->expander)) return NULL;
  return dtgtk_expander_get_frame(DTGTK_EXPANDER(module->gui->expander));
}

void dt_iop_gui_changed(dt_iop_module_t *action, GtkWidget *widget, gpointer data)
{
  if(IS_NULL_PTR(action)) return;
  dt_iop_module_t *module = (dt_iop_module_t *)action;

  if(module->gui_changed) module->gui_changed(module, widget, data);

  dt_iop_color_picker_reset(module, TRUE);

  dt_dev_add_history_item(module->dev, module, TRUE, TRUE);

  if(!IS_NULL_PTR(widget) && g_object_get_data(G_OBJECT(widget), "dt-blendop-header-update"))
    dt_iop_gui_update_header(module);
  else
    dt_iop_gui_set_enable_button(module);
}


void dt_bauhaus_update_module(dt_iop_module_t *self)
{
  dt_gui_module_t *m = DT_GUI_MODULE(self);

  for(GList *w = g_list_first(m->widget_list_bh); w; w = g_list_next(w))
  {
    GtkWidget *widget = (GtkWidget *)w->data;
    struct dt_bauhaus_widget_t *bhw = DT_BAUHAUS_WIDGET(widget);
    if(IS_NULL_PTR(bhw)) continue;

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

void dt_bauhaus_value_changed_default_callback(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_iop_module_t *module = (dt_iop_module_t *)w->module;
  if(IS_NULL_PTR(w->field) || IS_NULL_PTR(module)) return;

  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      float val = dt_bauhaus_slider_get(widget);
      switch(w->field_type)
      {
        case DT_INTROSPECTION_TYPE_FLOAT:
        {
          float *f = w->field, prevf = *f; *f = val;
          if(*f != prevf) dt_iop_gui_changed(module, widget, &prevf);
          break;
        }
        case DT_INTROSPECTION_TYPE_INT:
        {
          int *i = w->field, previ = *i; *i = val;
          if(*i != previ) dt_iop_gui_changed(module, widget, &previ);
          break;
        }
        case DT_INTROSPECTION_TYPE_USHORT:
        {
          unsigned short *s = w->field, prevs = *s; *s = val;
          if(*s != prevs) dt_iop_gui_changed(module, widget, &prevs);
          break;
        }
        default:
          fprintf(stderr, "[_bauhaus_slider_value_change] unsupported slider data type\n");
      }
      break;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      switch(w->field_type)
      {
        case DT_INTROSPECTION_TYPE_ENUM:
        {
          if(d->active >= 0)
          {
            const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
            int *e = w->field, preve = *e; *e = GPOINTER_TO_INT(entry->data);
            if(*e != preve) dt_iop_gui_changed(module, widget, &preve);
          }
          break;
        }
        case DT_INTROSPECTION_TYPE_INT:
        {
          int *i = w->field, previ = *i; *i = d->active;
          if(*i != previ) dt_iop_gui_changed(module, widget, &previ);
          break;
        }
        case DT_INTROSPECTION_TYPE_UINT:
        {
          unsigned int *u = w->field, prevu = *u; *u = d->active;
          if(*u != prevu) dt_iop_gui_changed(module, widget, &prevu);
          break;
        }
        case DT_INTROSPECTION_TYPE_BOOL:
        {
          gboolean *b = w->field, prevb = *b; *b = d->active;
          if(*b != prevb) dt_iop_gui_changed(module, widget, &prevb);
          break;
        }
        default:
          fprintf(stderr, "[_bauhaus_combobox_set] unsupported combo data type\n");
      }
      break;
    }
    default:
      fprintf(stderr, "[dt_bauhaus_value_changed_default_callback] invalid bauhaus widget type encountered for %s %s: %i\n", w->label, w->module->name, w->type);
  }
}
void dt_iop_gui_enter_critical_section(dt_iop_module_t *const module)
{
  // conditional on module->gui and paired with a dt_iop_gui_leave_critical_section() call at
  // an unrelated call site, which the thread-safety analyzer can't model -- same reason
  // dt_opencl_reserve_device_by_id()/dt_opencl_release_device() use the BAD variants.
  if(module->gui) dt_pthread_mutex_BAD_lock(&module->gui->gui_lock);
}

void dt_iop_gui_leave_critical_section(dt_iop_module_t *const module)
{
  if(module->gui) dt_pthread_mutex_BAD_unlock(&module->gui->gui_lock);
}

GtkWidget *dt_iop_gui_get_off(dt_iop_module_t *module)
{
  return module && module->gui ? module->gui->off : NULL;
}

gboolean dt_iop_gui_owns_widget(const dt_iop_module_t *module, const GtkWidget *target)
{
  return module->gui && (module->gui->expander == (const void *)target || module->gui->header == (const void *)target);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

/* Attached to each reset label; freed with the widget. */
typedef struct dt_iop_reset_label_t
{
  dt_iop_module_t *module;
  int offset; // offset of the parameter inside module->params
  int size;   // its size in bytes
} dt_iop_reset_label_t;

static void _iop_reset_label_reset(GtkWidget *widget, gpointer user_data)
{
  dt_iop_reset_label_t *d = (dt_iop_reset_label_t *)user_data;
  if(IS_NULL_PTR(d) || IS_NULL_PTR(d->module)) return;

  memcpy(((char *)d->module->params) + d->offset, ((char *)d->module->default_params) + d->offset, d->size);
  if(d->module->gui_update) d->module->gui_update(d->module);
  dt_dev_add_history_item(dt_dev_get_global(), d->module, FALSE, TRUE);
}

GtkWidget *dt_iop_gui_reset_label_new(const gchar *label, dt_iop_module_t *module, void *param,
                                      int param_size)
{
  GtkWidget *w = dtgtk_reset_label_new(label);

  dt_iop_reset_label_t *d = (dt_iop_reset_label_t *)g_malloc0(sizeof(dt_iop_reset_label_t));
  d->module = module;
  d->offset = param - (void *)module->params;
  d->size = param_size;

  g_signal_connect_data(G_OBJECT(w), "reset", G_CALLBACK(_iop_reset_label_reset), d,
                        (GClosureNotify)g_free, 0);
  return w;
}
