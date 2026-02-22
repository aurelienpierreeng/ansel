/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2011, 2013-2014 johannes hanika.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2011 Alexandre Prokoudine.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Jérémy Rosen.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2019 Tobias Ellinghaus.
    Copyright (C) 2012 Frédéric Grollier.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2014, 2020-2022 Aldric Renaudin.
    Copyright (C) 2013 Pascal de Bruijn.
    Copyright (C) 2013-2017 Roman Lebedev.
    Copyright (C) 2014-2017, 2019-2022 Pascal Obry.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 Peter Budai.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2020, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 Hanno Schwalm.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019, 2021 luzpaz.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Marco.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2022 Nicolas Auffray.
    Copyright (C) 2023 Alynx Zhou.
    
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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/styles.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/masks.h"

#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "common/history.h"
#include <complex.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

typedef struct dt_lib_history_t
{
  GtkWidget *history_view;
  GtkListStore *history_store;
  GtkWidget *create_button;
  GtkWidget *compress_button;
  gboolean selection_reset;
} dt_lib_history_t;

typedef enum dt_history_view_column_t
{
  // This stores the "history end" cursor, i.e.:
  // - 0 means "original" (raw input, no history item applied),
  // - N (1..len) means apply the first N history items (dev->history is 0..N-1).
  DT_HISTORY_VIEW_COL_HISTORY_END = 0,
  DT_HISTORY_VIEW_COL_NUMBER,
  DT_HISTORY_VIEW_COL_LABEL,
  DT_HISTORY_VIEW_COL_ICON_NAME,
  DT_HISTORY_VIEW_COL_ENABLED,
  DT_HISTORY_VIEW_COL_TOOLTIP,
  DT_HISTORY_VIEW_COL_COUNT
} dt_history_view_column_t;

/* compress history stack */
static void _lib_history_compress_clicked_callback(GtkButton *widget, gpointer user_data);
static gboolean _lib_history_compress_pressed_callback(GtkWidget *widget, GdkEventButton *e, gpointer user_data);
static gboolean _lib_history_view_button_press_callback(GtkWidget *widget, GdkEventButton *e, gpointer user_data);
static void _lib_history_create_style_button_clicked_callback(GtkWidget *widget, gpointer user_data);
/* signal callback for history change */
static void _lib_history_change_callback(gpointer instance, gpointer user_data);
static void _lib_history_view_selection_changed(GtkTreeSelection *selection, gpointer user_data);
static gboolean _lib_history_view_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                                GtkTooltip *tooltip, gpointer user_data);
static void _lib_history_view_cell_set_foreground(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                                                  GtkTreeModel *model, GtkTreeIter *iter, gpointer data);

const char *name(struct dt_lib_module_t *self)
{
  return _("History of changes");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 900;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_history_t *d = (dt_lib_history_t *)g_malloc0(sizeof(dt_lib_history_t));
  self->data = (void *)d;

  d->selection_reset = FALSE;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->widget, "history-ui");

  d->history_store = gtk_list_store_new(DT_HISTORY_VIEW_COL_COUNT,
                                        G_TYPE_INT,     // history_end
                                        G_TYPE_STRING,  // number
                                        G_TYPE_STRING,  // label
                                        G_TYPE_STRING,  // icon-name
                                        G_TYPE_BOOLEAN, // enabled
                                        G_TYPE_STRING); // tooltip text

  d->history_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->history_store));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->history_view), FALSE);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(d->history_view), FALSE);

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->history_view));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_BROWSE);
  g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(_lib_history_view_selection_changed), self);
  g_signal_connect(G_OBJECT(d->history_view), "button-press-event", G_CALLBACK(_lib_history_view_button_press_callback), self);

  gtk_widget_set_has_tooltip(d->history_view, TRUE);
  g_signal_connect(G_OBJECT(d->history_view), "query-tooltip", G_CALLBACK(_lib_history_view_query_tooltip), self);

  GtkCellRenderer *renderer_num = gtk_cell_renderer_text_new();
  g_object_set(G_OBJECT(renderer_num), "xalign", 1.0, "family", "monospace", NULL);
  GtkTreeViewColumn *col_num = gtk_tree_view_column_new_with_attributes("n", renderer_num,
                                                                        "text", DT_HISTORY_VIEW_COL_NUMBER,
                                                                        NULL);
  gtk_tree_view_column_set_cell_data_func(col_num, renderer_num, _lib_history_view_cell_set_foreground, NULL, NULL);
  gtk_tree_view_column_set_sizing(col_num, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->history_view), col_num);

  GtkCellRenderer *renderer_label = gtk_cell_renderer_text_new();
  g_object_set(G_OBJECT(renderer_label), "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  GtkTreeViewColumn *col_label = gtk_tree_view_column_new_with_attributes("label", renderer_label,
                                                                          "text", DT_HISTORY_VIEW_COL_LABEL,
                                                                          NULL);
  gtk_tree_view_column_set_cell_data_func(col_label, renderer_label, _lib_history_view_cell_set_foreground, NULL, NULL);
  gtk_tree_view_column_set_expand(col_label, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->history_view), col_label);

  GtkCellRenderer *renderer_icon = gtk_cell_renderer_pixbuf_new();
  GtkTreeViewColumn *col_icon = gtk_tree_view_column_new_with_attributes("status", renderer_icon,
                                                                         "icon-name", DT_HISTORY_VIEW_COL_ICON_NAME,
                                                                         NULL);
  gtk_tree_view_column_set_sizing(col_icon, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->history_view), col_icon);

  GtkWidget *hhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  d->compress_button = dt_action_button_new(self, N_("compress history stack"), _lib_history_compress_clicked_callback, self,
                                            _("create a minimal history stack which produces the same image\n"
                                              "ctrl+click to truncate history to the selected item"), 0, 0);
  g_signal_connect(G_OBJECT(d->compress_button), "button-press-event", G_CALLBACK(_lib_history_compress_pressed_callback), self);

  /* add toolbar button for creating style */
  d->create_button = dtgtk_button_new(dtgtk_cairo_paint_styles, CPF_NONE, NULL);
  g_signal_connect(G_OBJECT(d->create_button), "clicked",
                   G_CALLBACK(_lib_history_create_style_button_clicked_callback), NULL);
  gtk_widget_set_name(d->create_button, "non-flat");
  gtk_widget_set_tooltip_text(d->create_button, _("create a style from the current history stack"));

  /* add buttons to buttonbox */
  gtk_box_pack_start(GTK_BOX(hhbox), d->compress_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hhbox), d->create_button, FALSE, FALSE, 0);

  /* add history list and buttonbox to widget */
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->history_view, 1, "plugins/darkroom/history/windowheight"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hhbox, FALSE, FALSE, 0);

  gtk_widget_show_all(self->widget);

  /* connect to history change signal for updating the history view */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(_lib_history_change_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_history_change_callback), self);
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;
  if(d && d->history_store) g_object_unref(d->history_store);
  g_free(self->data);
  self->data = NULL;
}

static const char *_history_icon_name(const gboolean enabled, const gboolean default_enabled, const gboolean always_on,
                                      const gboolean deprecated)
{
  if(always_on) return "emblem-readonly";
  if(deprecated) return "dialog-warning";
  if(default_enabled) return enabled ? "emblem-ok" : "process-stop";
  return enabled ? "emblem-ok" : "process-stop";
}

static gchar *_lib_history_change_text(dt_introspection_field_t *field, const char *d, gpointer params, gpointer oldpar)
{
  dt_iop_params_t *p = (dt_iop_params_t *)((uint8_t *)params + field->header.offset);
  dt_iop_params_t *o = (dt_iop_params_t *)((uint8_t *)oldpar + field->header.offset);

  switch(field->header.type)
  {
  case DT_INTROSPECTION_TYPE_STRUCT:
  case DT_INTROSPECTION_TYPE_UNION:
    {
      gchar **change_parts = g_malloc0_n(field->Struct.entries + 1, sizeof(char*));
      int num_parts = 0;

      for(int i = 0; i < field->Struct.entries; i++)
      {
        dt_introspection_field_t *entry = field->Struct.fields[i];

        gchar *description = _(*entry->header.description ?
                                entry->header.description :
                                entry->header.field_name);

        if(d) description = g_strdup_printf("%s.%s", d, description);

        if((change_parts[num_parts] = _lib_history_change_text(entry, description, params, oldpar)))
          num_parts++;

        if(d) g_free(description);
      }

      gchar *struct_text = num_parts ? g_strjoinv("\n", change_parts) : NULL;
      g_strfreev(change_parts);

      return struct_text;
    }
    break;
  case DT_INTROSPECTION_TYPE_ARRAY:
    if(field->Array.type == DT_INTROSPECTION_TYPE_CHAR)
    {
      const gboolean is_valid =
        g_utf8_validate((char *)o, -1, NULL)
        && g_utf8_validate((char *)p, -1, NULL);

      if(is_valid && strncmp((char*)o, (char*)p, field->Array.count))
        return g_strdup_printf("%s\t\"%s\"\t\u2192\t\"%s\"", d, (char*)o, (char*)p);
    }
    else
    {
      const int max_elements = 4;
      gchar **change_parts = g_malloc0_n(max_elements + 1, sizeof(char*));
      int num_parts = 0;

      for(int i = 0, item_offset = 0; i < field->Array.count; i++, item_offset += field->Array.field->header.size)
      {
        char *description = g_strdup_printf("%s[%d]", d, i);
        char *element_text = _lib_history_change_text(field->Array.field, description, (uint8_t *)params + item_offset, (uint8_t *)oldpar + item_offset);
        g_free(description);

        if(element_text && ++num_parts <= max_elements)
          change_parts[num_parts - 1] = element_text;
        else
          g_free(element_text);
      }

      gchar *array_text = NULL;
      if(num_parts > max_elements)
        array_text = g_strdup_printf("%s\t%d changes", d, num_parts);
      else if(num_parts > 0)
        array_text = g_strjoinv("\n", change_parts);

      g_strfreev(change_parts);

      return array_text;
    }
    break;
  case DT_INTROSPECTION_TYPE_FLOAT:
    if(*(float*)o != *(float*)p && (isfinite(*(float*)o) || isfinite(*(float*)p)))
      return g_strdup_printf("%s\t%.4f\t\u2192\t%.4f", d, *(float*)o, *(float*)p);
    break;
  case DT_INTROSPECTION_TYPE_INT:
    if(*(int*)o != *(int*)p)
      return g_strdup_printf("%s\t%d\t\u2192\t%d", d, *(int*)o, *(int*)p);
    break;
  case DT_INTROSPECTION_TYPE_UINT:
    if(*(unsigned int*)o != *(unsigned int*)p)
      return g_strdup_printf("%s\t%u\t\u2192\t%u", d, *(unsigned int*)o, *(unsigned int*)p);
    break;
  case DT_INTROSPECTION_TYPE_USHORT:
    if(*(unsigned short int*)o != *(unsigned short int*)p)
      return g_strdup_printf("%s\t%hu\t\u2192\t%hu", d, *(unsigned short int*)o, *(unsigned short int*)p);
    break;
  case DT_INTROSPECTION_TYPE_INT8:
    if(*(uint8_t*)o != *(uint8_t*)p)
      return g_strdup_printf("%s\t%d\t\u2192\t%d", d, *(uint8_t*)o, *(uint8_t*)p);
    break;
  case DT_INTROSPECTION_TYPE_CHAR:
    if(*(char*)o != *(char*)p)
      return g_strdup_printf("%s\t'%c'\t\u2192\t'%c'", d, *(char *)o, *(char *)p);
    break;
  case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
    if(*(float complex*)o != *(float complex*)p)
      return g_strdup_printf("%s\t%.4f + %.4fi\t\u2192\t%.4f + %.4fi", d,
                             creal(*(float complex*)o), cimag(*(float complex*)o),
                             creal(*(float complex*)p), cimag(*(float complex*)p));
    break;
  case DT_INTROSPECTION_TYPE_ENUM:
    if(*(int*)o != *(int*)p)
    {
      const char *old_str = N_("unknown"), *new_str = N_("unknown");
      for(dt_introspection_type_enum_tuple_t *i = field->Enum.values; i && i->name; i++)
      {
        if(i->value == *(int*)o)
        {
          old_str = i->description;
          if(!*old_str) old_str = i->name;
        }
        if(i->value == *(int*)p)
        {
          new_str = i->description;
          if(!*new_str) new_str = i->name;
        }
      }

      return g_strdup_printf("%s\t%s\t\u2192\t%s", d, _(old_str), _(new_str));
    }
    break;
  case DT_INTROSPECTION_TYPE_BOOL:
    if(*(gboolean*)o != *(gboolean*)p)
    {
      char *old_str = *(gboolean*)o ? "on" : "off";
      char *new_str = *(gboolean*)p ? "on" : "off";
      return g_strdup_printf("%s\t%s\t\u2192\t%s", d, _(old_str), _(new_str));
    }
    break;
  case DT_INTROSPECTION_TYPE_OPAQUE:
    {
      // TODO: special case float2
    }
    break;
  default:
    fprintf(stderr, "unsupported introspection type \"%s\" encountered in _lib_history_change_text (field %s)\n", field->header.type_name, field->header.field_name);
    break;
  }

  return NULL;
}

static const dt_dev_history_item_t * _find_previous_history_step(const dt_dev_history_item_t *hitem)
{
  // Find the immediate previous history instance matching the current module
  for(const GList *find_old = g_list_find(darktable.develop->history, hitem);
      find_old;
      find_old = g_list_previous(find_old))
  {
    dt_dev_history_item_t *hprev = (dt_dev_history_item_t *)(find_old->data);
    if(hprev == hitem) continue; // first loop run, we start from current hitem
    if(hprev->module == hitem->module) return hprev;
  }

  // This is the first history element for this module
  return hitem;
}

#define add_blend_history_change(field, format, label)                                                            \
  if((hitem->blend_params->field) != (old_blend->field))                                                          \
  {                                                                                                               \
    gchar *full_format = g_strconcat("%s\t", format, "\t\u2192\t", format, NULL);                                 \
    change_parts[num_parts++]                                                                                     \
        = g_strdup_printf(full_format, label, (old_blend->field), (hitem->blend_params->field));                  \
    g_free(full_format);                                                                                          \
  }

#define add_blend_history_change_enum(field, label, list)                                                         \
  if((hitem->blend_params->field) != (old_blend->field))                                                          \
  {                                                                                                               \
    const char *old_str = NULL, *new_str = NULL;                                                                  \
    for(const dt_develop_name_value_t *i = list; *i->name; i++)                                                   \
    {                                                                                                             \
      if(i->value == (old_blend->field)) old_str = i->name;                                                       \
      if(i->value == (hitem->blend_params->field)) new_str = i->name;                                             \
    }                                                                                                             \
                                                                                                                  \
    change_parts[num_parts++]                                                                                     \
        = (!old_str || !new_str)                                                                                  \
              ? g_strdup_printf("%s\t%d\t\u2192\t%d", label, old_blend->field, hitem->blend_params->field)        \
              : g_strdup_printf("%s\t%s\t\u2192\t%s", label, _(g_dpgettext2(NULL, "blendmode", old_str)),         \
                                _(g_dpgettext2(NULL, "blendmode", new_str)));                                     \
  }

#define add_history_change(field, format, label)                                                                  \
  if((hitem->field) != (hprev->field))                                                                            \
  {                                                                                                               \
    gchar *full_format = g_strconcat("%s\t", format, "\t\u2192\t", format, NULL);                                 \
    change_parts[num_parts++] = g_strdup_printf(full_format, label, (hprev->field), (hitem->field));              \
    g_free(full_format);                                                                                          \
  }

#define add_history_change_string(field, label)                                                                   \
  if(strcmp(hitem->field, hprev->field))                                                                          \
  {                                                                                                               \
    change_parts[num_parts++]                                                                                     \
        = g_strdup_printf("%s\t\"%s\"\t\u2192\t\"%s\"", label, (hprev->field), (hitem->field));                   \
  }

#define add_history_change_boolean(field, label)                                                                  \
  if((hitem->field) != (hprev->field))                                                                            \
  {                                                                                                               \
    change_parts[num_parts++] = g_strdup_printf("%s\t%s\t\u2192\t%s", label, (hprev->field) ? _("on") : _("off"), \
                                                (hitem->field) ? _("on") : _("off"));                             \
  }


static gchar *_create_tooltip_text(const dt_dev_history_item_t *hitem)
{
  if(!hitem || !hitem->module) return NULL;

  const dt_dev_history_item_t *hprev = _find_previous_history_step(hitem);
  dt_iop_params_t *old_params
      = (hprev == hitem || hprev == NULL) ? hitem->module->default_params : hprev->module->params;
  dt_develop_blend_params_t *old_blend
      = (hprev == hitem || hprev == NULL) ? hitem->module->default_blendop_params : hprev->module->blend_params;

  gchar **change_parts = g_malloc0_n(sizeof(dt_develop_blend_params_t) / (sizeof(float)) + 24, sizeof(char*));
  int num_parts = 0;

  const gboolean enabled_by_default = (hitem->module->force_enable && hitem->module->force_enable(hitem->module, hitem->enabled))
                                      || hitem->module->default_enabled;
  if(hprev == hitem)
  {
    // This is the first history entry for this module.
    // That means the module was necessarily enabled in this step.
    if(enabled_by_default)
      change_parts[num_parts++] = g_strdup_printf(_("mandatory module created automatically"));
    else
      change_parts[num_parts++] = g_strdup_printf(_("module created per user request"));
  }
  else
  {
    // This is not the first history entry for this module.
    // It can have been disabled.
    add_history_change_boolean(enabled, _("enabled"));
  }

  add_history_change(iop_order, "%i", _("pipeline order"));
  add_history_change_string(multi_name, _("instance name"));

  if(hitem->module->have_introspection)
    change_parts[num_parts++] = _lib_history_change_text(hitem->module->get_introspection()->field, NULL,
                                                hitem->params, old_params);

  if(hitem->module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    add_blend_history_change_enum(blend_cst, _("colorspace"), dt_develop_blend_colorspace_names);
    add_blend_history_change_enum(mask_mode, _("mask mode"), dt_develop_mask_mode_names);
    add_blend_history_change_enum(blend_mode & DEVELOP_BLEND_MODE_MASK, _("blend mode"), dt_develop_blend_mode_names);
    add_blend_history_change_enum(blend_mode & DEVELOP_BLEND_REVERSE, _("blend operation"), dt_develop_blend_mode_flag_names);
    add_blend_history_change(blend_parameter, _("%.2f EV"), _("blend fulcrum"));
    add_blend_history_change(opacity, "%.4f", _("mask opacity"));
    add_blend_history_change_enum(mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL), _("combine masks"), dt_develop_combine_masks_names);
    add_blend_history_change(feathering_radius, "%.4f", _("feathering radius"));
    add_blend_history_change_enum(feathering_guide, _("feathering guide"), dt_develop_feathering_guide_names);
    add_blend_history_change(blur_radius, "%.4f", _("mask blur"));
    add_blend_history_change(contrast, "%.4f", _("mask contrast"));
    add_blend_history_change(brightness, "%.4f", _("brightness"));
    add_blend_history_change(raster_mask_instance, "%d", _("raster mask instance"));
    add_blend_history_change(raster_mask_id, "%d", _("raster mask id"));
    add_blend_history_change_enum(raster_mask_invert, _("invert mask"), dt_develop_invert_mask_names);

    add_blend_history_change(mask_combine & DEVELOP_COMBINE_MASKS_POS ? '-' : '+', "%c", _("drawn mask polarity"));

    if(hitem->blend_params->mask_id != old_blend->mask_id)
      change_parts[num_parts++] = old_blend->mask_id == 0
                                ? g_strdup_printf(_("a drawn mask was added"))
                                : hitem->blend_params->mask_id == 0
                                ? g_strdup_printf(_("the drawn mask was removed"))
                                : g_strdup_printf(_("the drawn mask was changed"));

    dt_iop_gui_blend_data_t *bd = hitem->module->blend_data;

    for(int in_out = 1; in_out >= 0; in_out--)
    {
      gboolean first = TRUE;

      for(const dt_iop_gui_blendif_channel_t *b = bd ? bd->channel : NULL;
          b && b->label != NULL;
          b++)
      {
        const dt_develop_blendif_channels_t ch = b->param_channels[in_out];

        const int oactive = old_blend->blendif & (1 << ch);
        const int nactive = hitem->blend_params->blendif & (1 << ch);

        const int opolarity = old_blend->blendif & (1 << (ch + 16));
        const int npolarity = hitem->blend_params->blendif & (1 << (ch + 16));

        float *of = &old_blend->blendif_parameters[4 * ch];
        float *nf = &hitem->blend_params->blendif_parameters[4 * ch];

        const float oboost = exp2f(old_blend->blendif_boost_factors[ch]);
        const float nboost = exp2f(hitem->blend_params->blendif_boost_factors[ch]);

        if((oactive || nactive) && (memcmp(of, nf, sizeof(float) * 4) || opolarity != npolarity))
        {
          if(first)
          {
            change_parts[num_parts++] = g_strdup(in_out ? _("parametric output mask:") : _("parametric input mask:"));
            first = FALSE;
          }
          char s[4][2][25];
          for(int k = 0; k < 4; k++)
          {
            b->scale_print(of[k], oboost, s[k][0], sizeof(s[k][0]));
            b->scale_print(nf[k], nboost, s[k][1], sizeof(s[k][1]));
          }

          char *opol = !oactive ? "" : (opolarity ? "(-)" : "(+)");
          char *npol = !nactive ? "" : (npolarity ? "(-)" : "(+)");

          change_parts[num_parts++] = g_strdup_printf("%s\t%s| %s- %s| %s%s\t\u2192\t%s| %s- %s| %s%s", _(b->name),
                                                      s[0][0], s[1][0], s[2][0], s[3][0], opol,
                                                      s[0][1], s[1][1], s[2][1], s[3][1], npol);
        }
      }
    }
  }

  gchar *tooltip_text = g_strjoinv("\n", change_parts);
  g_strfreev(change_parts);

  return tooltip_text;
}

static gboolean _changes_tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                          GtkTooltip *tooltip)
{
  const gchar *tooltip_text = g_object_get_data(G_OBJECT(widget), "tooltip-text");
  if(!tooltip_text || !tooltip_text[0]) return FALSE;

  static GtkWidget *view = NULL;
  if(!view)
  {
    view = gtk_text_view_new();
    dt_gui_add_class(view, "dt_transparent_background");
    dt_gui_add_class(view, "dt_monospace");
    g_signal_connect(G_OBJECT(view), "destroy", G_CALLBACK(gtk_widget_destroyed), &view);
  }

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  gtk_text_buffer_set_text(buffer, tooltip_text, -1);
  gtk_tooltip_set_custom(tooltip, view);
  gtk_widget_map(view); // FIXME: workaround added in order to fix #9908, probably a Gtk issue, remove when fixed upstream

  int count_column1 = 0, count_column2 = 0;
  for(gchar *line = (gchar *)tooltip_text; *line;)
  {
    gchar *endline = g_strstr_len(line, -1, "\n");
    if(!endline) endline = line + strlen(line);

    gchar *found_tab1 = g_strstr_len(line, endline - line, "\t");
    if(found_tab1)
    {
      if(found_tab1 - line >= count_column1) count_column1 = found_tab1 - line + 1;

      gchar *found_tab2 = g_strstr_len(found_tab1 + 1, endline - found_tab1 - 1, "\t");
      if(found_tab2 - found_tab1 > count_column2) count_column2 = found_tab2 - found_tab1;
    }

    line = endline;
    if(*line) line++;
  }

  PangoLayout *layout = gtk_widget_create_pango_layout(view, " ");
  int char_width;
  pango_layout_get_size(layout, &char_width, NULL);
  g_object_unref(layout);
  PangoTabArray *tabs = pango_tab_array_new_with_positions(3, FALSE, PANGO_TAB_LEFT, (count_column1) * char_width,
                                                                      PANGO_TAB_LEFT, (count_column1 + count_column2) * char_width,
                                                                      PANGO_TAB_LEFT, (count_column1 + count_column2 + 2) * char_width);
  gtk_text_view_set_tabs(GTK_TEXT_VIEW(view), tabs);
  pango_tab_array_free(tabs);

  return TRUE;
}

static gboolean _lib_history_view_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                                GtkTooltip *tooltip, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;
  GtkTreeModel *model = GTK_TREE_MODEL(d->history_store);
  GtkTreeIter iter;
  GtkTreePath *path = NULL;

  if(keyboard_mode)
  {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
    if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return FALSE;
  }
  else
  {
    if(!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), x, y, &path, NULL, NULL, NULL)) return FALSE;
    if(!gtk_tree_model_get_iter(model, &iter, path))
    {
      gtk_tree_path_free(path);
      return FALSE;
    }
  }

  gchar *tooltip_text = NULL;
  gtk_tree_model_get(model, &iter, DT_HISTORY_VIEW_COL_TOOLTIP, &tooltip_text, -1);
  g_object_set_data_full(G_OBJECT(widget), "tooltip-text", tooltip_text, g_free);

  const gboolean ret = _changes_tooltip_callback(widget, x, y, keyboard_mode, tooltip);
  if(path) gtk_tree_path_free(path);
  return ret;
}

static void _lib_history_view_cell_set_foreground(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                                                  GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  gboolean enabled = TRUE;
  gtk_tree_model_get(model, iter, DT_HISTORY_VIEW_COL_ENABLED, &enabled, -1);
  if(enabled)
    g_object_set(G_OBJECT(renderer), "foreground-set", FALSE, NULL);
  else
    g_object_set(G_OBJECT(renderer), "foreground-set", TRUE, "foreground", "#888", NULL);
}

static void _history_apply_history_end(const int history_end)
{
  dt_develop_t *dev = darktable.develop;

  dt_dev_undo_start_record(dev);

  if(darktable.gui && dev->gui_attached) ++darktable.gui->reset;
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_set_history_end_ext(dev, history_end);
  dt_dev_pop_history_items_ext(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  if(darktable.gui && dev->gui_attached) --darktable.gui->reset;

  dt_dev_undo_end_record(dev);

  dt_dev_write_history(dev);
  dt_dev_history_gui_update(dev);
  dt_dev_history_pixelpipe_update(dev, FALSE);
  dt_dev_history_notify_change(dev, dev->image_storage.id);
}

static void _history_show_module_for_end(const int history_end)
{
  if(history_end <= 0) return;

  dt_pthread_rwlock_rdlock(&darktable.develop->history_mutex);
  dt_dev_history_item_t *hist
      = (dt_dev_history_item_t *)g_list_nth_data(darktable.develop->history, history_end - 1);
  dt_iop_module_t *module = hist ? hist->module : NULL;
  dt_pthread_rwlock_unlock(&darktable.develop->history_mutex);
  if(module)
  {
    dt_dev_modulegroups_switch(darktable.develop, module);
    dt_iop_gui_set_expanded(module, TRUE, TRUE);
  }
}

static void _history_store_add_original(dt_lib_history_t *d)
{
  GtkTreeIter iter;
  gtk_list_store_append(d->history_store, &iter);
  gtk_list_store_set(d->history_store, &iter, DT_HISTORY_VIEW_COL_HISTORY_END, 0, DT_HISTORY_VIEW_COL_NUMBER, " 0",
                     DT_HISTORY_VIEW_COL_LABEL, _("original"), DT_HISTORY_VIEW_COL_ICON_NAME,
                     _history_icon_name(TRUE, FALSE, TRUE, FALSE), DT_HISTORY_VIEW_COL_ENABLED, TRUE,
                     DT_HISTORY_VIEW_COL_TOOLTIP, "", -1);
}

static gchar *_history_tooltip_with_hint(const dt_dev_history_item_t *hitem)
{
  const gchar *hint = _("Shift+click: show module without changing history");

  gchar *tooltip_text = _create_tooltip_text(hitem);
  if(tooltip_text && tooltip_text[0])
  {
    gchar *tooltip_with_hint = g_strconcat(tooltip_text, "\n\n", hint, NULL);
    g_free(tooltip_text);
    return tooltip_with_hint;
  }

  g_free(tooltip_text);
  return g_strdup(hint);
}

static void _history_store_prepend_item(dt_lib_history_t *d, const dt_dev_history_item_t *hitem, const int history_end)
{
  const gboolean enabled = (hitem->enabled || (strcmp(hitem->op_name, "mask_manager") == 0));
  const gboolean deprecated = (hitem->module->flags() & IOP_FLAGS_DEPRECATED);
  const char *icon_name = _history_icon_name(enabled, hitem->module->default_enabled, hitem->module->hide_enable_button,
                                             deprecated);

  const gboolean enabled_by_default
      = ((hitem->module->force_enable && hitem->module->force_enable(hitem->module, hitem->enabled))
         || hitem->module->default_enabled);
  const gchar *star = (hitem == _find_previous_history_step(hitem) && enabled_by_default) ? " *" : "";

  gchar *clean_name = delete_underscore(hitem->module->name());
  gchar *label = NULL;
  if(!hitem->multi_name[0] || strcmp(hitem->multi_name, "0") == 0)
    label = g_strdup_printf("%s%s", clean_name, star);
  else
    label = g_strdup_printf("%s %s%s", clean_name, hitem->multi_name, star);
  g_free(clean_name);

  gchar number[10];
  g_snprintf(number, sizeof(number), "%2d", history_end);

  gchar *tooltip_text = _history_tooltip_with_hint(hitem);

  GtkTreeIter iter;
  gtk_list_store_insert(d->history_store, &iter, 0);
  gtk_list_store_set(d->history_store, &iter, DT_HISTORY_VIEW_COL_HISTORY_END, history_end,
                     DT_HISTORY_VIEW_COL_NUMBER, number, DT_HISTORY_VIEW_COL_LABEL, label,
                     DT_HISTORY_VIEW_COL_ICON_NAME, icon_name, DT_HISTORY_VIEW_COL_ENABLED, enabled,
                     DT_HISTORY_VIEW_COL_TOOLTIP, tooltip_text ? tooltip_text : "", -1);

  g_free(tooltip_text);
  g_free(label);
}

static void _history_select_row_for_end(dt_lib_history_t *d, const int history_end)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->history_view));
  GtkTreeModel *model = GTK_TREE_MODEL(d->history_store);
  GtkTreeIter iter;

  for(gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter))
  {
    int row_history_end = 0;
    gtk_tree_model_get(model, &iter, DT_HISTORY_VIEW_COL_HISTORY_END, &row_history_end, -1);
    if(row_history_end == history_end)
    {
      gtk_tree_selection_select_iter(selection, &iter);
      return;
    }
  }
}

static void _lib_history_change_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;

  d->selection_reset = TRUE;
  gtk_list_store_clear(d->history_store);

  // Read-only access: don't take a write lock here. This callback can run
  // while the pixelpipe holds a read lock, and a write lock would deadlock
  // the UI thread when history change signals are emitted.
  dt_pthread_rwlock_rdlock(&darktable.develop->history_mutex);

  _history_store_add_original(d);

  int history_pos = 0;
  for(const GList *history = darktable.develop->history; history; history = g_list_next(history), history_pos++)
    _history_store_prepend_item(d, (const dt_dev_history_item_t *)history->data, history_pos + 1);

  const int history_end = dt_dev_get_history_end_ext(darktable.develop);
  dt_pthread_rwlock_unlock(&darktable.develop->history_mutex);

  _history_select_row_for_end(d, history_end);
  d->selection_reset = FALSE;
}

static void _lib_history_truncate()
{
  dt_develop_t *dev = darktable.develop;
  const int32_t imgid = dev->image_storage.id;

  dt_dev_undo_start_record(dev);

  if(dt_dev_get_history_end_ext(dev) == g_list_length(dev->history))
    dt_dev_history_compress(dev);
  else
    dt_history_truncate_on_image(dev, imgid, dt_dev_get_history_end_ext(dev));

  dev->proxy.chroma_adaptation = NULL;
  dt_dev_undo_end_record(dev);

  int pipe_remove = dt_dev_history_refresh_nodes(dev, dev->iop, dev->history);
  dt_dev_history_gui_update(dev);
  dt_dev_history_pixelpipe_update(dev, pipe_remove);
  dt_dev_history_notify_change(dev, dev->image_storage.id);
}


static void _lib_history_compress_clicked_callback(GtkButton *widget, gpointer user_data)
{
  _lib_history_truncate();
}

static gboolean _lib_history_compress_pressed_callback(GtkWidget *widget, GdkEventButton *e, gpointer user_data)
{
  _lib_history_truncate();

  return TRUE;
}

static void _lib_history_view_selection_changed(GtkTreeSelection *selection, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_history_t *d = (dt_lib_history_t *)self->data;
  if(!d || d->selection_reset || darktable.gui->reset) return;

  GtkTreeModel *model = NULL;
  GtkTreeIter iter;
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  int history_end = 0;
  gtk_tree_model_get(model, &iter, DT_HISTORY_VIEW_COL_HISTORY_END, &history_end, -1);
  if(history_end == dt_dev_get_history_end_ext(darktable.develop)) return;

  _history_apply_history_end(history_end);
}

static gboolean _lib_history_view_button_press_callback(GtkWidget *widget, GdkEventButton *e, gpointer user_data)
{
  // shift-click just shows the corresponding module in modulegroups
  if(e->button == 1 && dt_modifier_is(e->state, GDK_SHIFT_MASK))
  {
    GtkTreePath *path = NULL;
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)e->x, (gint)e->y, &path, NULL, NULL, NULL))
    {
      dt_lib_module_t *self = (dt_lib_module_t *)user_data;
      dt_lib_history_t *d = (dt_lib_history_t *)self->data;
      GtkTreeModel *model = GTK_TREE_MODEL(d->history_store);
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
      {
        int history_end = 0;
        gtk_tree_model_get(model, &iter, DT_HISTORY_VIEW_COL_HISTORY_END, &history_end, -1);
        _history_show_module_for_end(history_end);
      }
      gtk_tree_path_free(path);
      return TRUE;
    }
  }

  return FALSE;
}

static void _lib_history_create_style_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  if(darktable.develop->image_storage.id)
  {
    dt_dev_write_history(darktable.develop);
    dt_gui_styles_dialog_new(darktable.develop->image_storage.id);
  }
}

void gui_reset(dt_lib_module_t *self)
{
  const int32_t imgid = darktable.develop->image_storage.id;
  if(!imgid) return;

  gint res = GTK_RESPONSE_YES;

  if(dt_conf_get_bool("ask_before_discard"))
  {
    const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        _("do you really want to clear history of current image?"));
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), _("delete image's history?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  if(res == GTK_RESPONSE_YES)
  {
    dt_dev_undo_start_record(darktable.develop);

    dt_history_delete_on_image_ext(imgid, FALSE);

    dt_dev_undo_end_record(darktable.develop);

    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));

    dt_dev_pixelpipe_refresh_all(darktable.develop, TRUE);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
