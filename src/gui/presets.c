/*
    This file is part of darktable,
    Copyright (C) 2010 Bruce Guenter.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010-2014, 2018 johannes hanika.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Brian Teague.
    Copyright (C) 2011-2012 Jérémy Rosen.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2016, 2018-2019 Tobias Ellinghaus.
    Copyright (C) 2011-2013 Ulrich Pegelow.
    Copyright (C) 2012, 2020-2021 Aldric Renaudin.
    Copyright (C) 2012 Ivan Tarozzi.
    Copyright (C) 2012 José Carlos García Sogo.
    Copyright (C) 2012, 2017-2018, 2020 parafin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013, 2018-2021 Pascal Obry.
    Copyright (C) 2013-2017 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2014 Pascal de Bruijn.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2017-2018, 2021 Heiko Bauke.
    Copyright (C) 2019, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2019 Jakub Filipowicz.
    Copyright (C) 2019 Sam Smith.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020 darkelectron.
    Copyright (C) 2020-2021 Diederik Ter Rahe.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Tomasz Golinski.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Marco.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2021-2022 Nicolas Auffray.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "common/conf.h"
#endif
#include "widgets/bauhaus.h"
#include "database/preset_repository.h"
#include "common/presets.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"

#include "gui/application.h"
#include "gui/presets.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <assert.h>
#include "widgets/dialog.h"
#include "widgets/widget_style.h"

const int dt_gui_presets_exposure_value_cnt = 24;
const float dt_gui_presets_exposure_value[]
    = { 0.,       1. / 8000, 1. / 4000, 1. / 2000, 1. / 1000, 1. / 1000, 1. / 500, 1. / 250,
        1. / 125, 1. / 60,   1. / 30,   1. / 15,   1. / 15,   1. / 8,    1. / 4,   1. / 2,
        1,        2,         4,         8,         15,        30,        60,       FLT_MAX };
const char *dt_gui_presets_exposure_value_str[]
    = { "0",     "1/8000", "1/4000", "1/2000", "1/1000", "1/1000", "1/500", "1/250",
        "1/125", "1/60",   "1/30",   "1/15",   "1/15",   "1/8",    "1/4",   "1/2",
        "1\"",   "2\"",    "4\"",    "8\"",    "15\"",   "30\"",   "60\"",  "+" };
const int dt_gui_presets_aperture_value_cnt = 19;
const float dt_gui_presets_aperture_value[]
    = { 0, 0.5, 0.7, 1.0, 1.4, 2.0, 2.8, 4.0, 5.6, 8.0, 11.0, 16.0, 22.0, 32.0, 45.0, 64.0, 90.0, 128.0, FLT_MAX };
const char *dt_gui_presets_aperture_value_str[]
    = { "f/0",  "f/0.5", "f/0.7", "f/1.0", "f/1.4", "f/2",  "f/2.8", "f/4",   "f/5.6", "f/8",
        "f/11", "f/16",  "f/22",  "f/32",  "f/45",  "f/64", "f/90",  "f/128", "f/+" };

static gboolean _gui_presets_autogen_enabled = TRUE;

// format string and corresponding flag stored into the database
static const char *_gui_presets_format_value_str[5]
    = { N_("non-raw"), N_("raw"), N_("HDR"), N_("monochrome"), N_("color") };
static const int _gui_presets_format_flag[5] = { FOR_LDR, FOR_RAW, FOR_HDR, FOR_NOT_MONO, FOR_NOT_COLOR };

// this is also called for non-gui applications linking to libansel!
// so beware, don't use any dt_gui_get_global() stuff here .. (or change this behaviour in darktable.c)
void dt_gui_presets_init()
{
  // Avoid regenerating all auto-presets on every startup when the build and UI language are unchanged.
  // This cuts a large number of INSERTs during module load without altering behavior across upgrades.
  gchar *lang = dt_conf_get_string("ui_last/gui_language");
  if(IS_NULL_PTR(lang)) lang = g_strdup("");
  gchar *sig = g_strdup_printf("%s|%s", darktable_package_version, lang);
  gchar *prev = dt_conf_get_string("ui_last/presets_autogen_signature");
  _gui_presets_autogen_enabled = !(prev && !g_strcmp0(prev, sig));
  dt_conf_set_string("ui_last/presets_autogen_signature", sig);
  dt_free(prev);
  dt_free(sig);
  dt_free(lang);

  // remove auto generated presets from plugins, not the user included ones.
  if(_gui_presets_autogen_enabled)
  {
    dt_preset_repository_delete_shipped();
  }
}

gboolean dt_gui_presets_autogen_enabled()
{
  return _gui_presets_autogen_enabled;
}

void dt_gui_presets_cleanup()
{
  /* Every statement this used to finalise belongs to database/preset_repository.c now,
   * and is released with the rest of its cache. */
  dt_preset_repository_cleanup();
}

void dt_gui_presets_add_generic(const char *name, dt_dev_operation_t op, const int32_t version,
                                const void *params, const int32_t params_size,
                                const int32_t enabled,
                                const dt_develop_blend_colorspace_t blend_cst)
{
  dt_develop_blend_params_t default_blendop_params;
  dt_develop_blend_init_blend_parameters(&default_blendop_params, blend_cst);
  dt_gui_presets_add_with_blendop(
      name, op, version, params, params_size,
      &default_blendop_params, enabled);
}

void dt_gui_presets_add_with_blendop(
    const char *name, dt_dev_operation_t op, const int32_t version,
    const void *params, const int32_t params_size,
    const void *blend_params, const int32_t enabled)
{
  dt_preset_repository_add_iop_preset(name, op, version, params, params_size,
                                      blend_params, sizeof(dt_develop_blend_params_t),
                                      dt_develop_blend_version(), enabled);
}

static gchar *_get_active_preset_name(dt_iop_module_t *module, int *writeprotect)
{
  // if we sort by writeprotect DESC then in case user copied the writeprotected preset
  // then the preset name returned will be writeprotected and thus not deletable
  // sorting ASC prefers user created presets. (the repository does that ordering)
  GList *presets = dt_preset_repository_list_for_iop(module->op, module->version());

  gchar *name = NULL;
  for(GList *l = presets; l; l = g_list_next(l))
  {
    const dt_module_preset_t *p = (const dt_module_preset_t *)l->data;

    if(!memcmp(module->params, p->op_params, MIN(p->op_params_size, module->params_size))
       && !memcmp(module->blend_params, p->blendop_params,
                  MIN(p->blendop_params_size, (int)sizeof(dt_develop_blend_params_t)))
       && module->enabled == p->enabled)
    {
      name = g_strdup(p->name);
      *writeprotect = p->writeprotect;
      break;
    }
  }
  g_list_free_full(presets, dt_module_preset_free);
  return name;
}

static void _menuitem_delete_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  int writeprotect = -1;
  gchar *name = _get_active_preset_name(module, &writeprotect);
  if(IS_NULL_PTR(name)) return;

  if(writeprotect)
  {
    dt_control_log(_("preset `%s' is write-protected, can't delete!"), name);
    dt_free(name);
    return;
  }

  gint res = GTK_RESPONSE_YES;

  if(dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset"))
  {
    GtkWidget *window = dt_gui_main_window();
    GtkWidget *dialog
      = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_YES_NO, _("do you really want to delete the preset `%s'?"), name);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete preset?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    GtkWindow *dialog_parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
    gtk_widget_destroy(dialog);
    dt_gui_refocus_parent(dialog_parent);
  }

  if(res == GTK_RESPONSE_YES)
    dt_lib_presets_remove(name, module->op, module->version());

  dt_free(name);
}
static void _edit_preset_final_callback(dt_gui_presets_edit_dialog_t *g)
{
  dt_gui_store_last_preset(gtk_entry_get_text(g->name));
}

static void _edit_preset_response(GtkDialog *dialog, gint response_id, dt_gui_presets_edit_dialog_t *g)
{
  if(response_id == GTK_RESPONSE_OK)
  {
    // we want to save the preset in the database

    // we verify eventual name collisions
    const gchar *name = gtk_entry_get_text(g->name);
    if(((g->old_id >= 0) && (strcmp(g->original_name, name) != 0)) || (g->old_id < 0))
    {
      if(IS_NULL_PTR(name) || *name == '\0' || strcmp(_("new preset"), name) == 0)
      {
        // show error dialog
        GtkWidget *dlg_changename
            = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, _("please give preset a name"));
#ifdef GDK_WINDOWING_QUARTZ
        dt_osx_disallow_fullscreen(dlg_changename);
#endif

        gtk_window_set_title(GTK_WINDOW(dlg_changename), _("unnamed preset"));

        gtk_dialog_run(GTK_DIALOG(dlg_changename));
        gtk_widget_destroy(dlg_changename);
        return;
      }

      // editing existing preset with different name or store new preset -> check for a preset with the same
      // name:
      if(dt_preset_repository_module_preset_exists(g->operation, g->op_version, name))
      {
        // show overwrite question dialog
        GtkWidget *dlg_overwrite = gtk_message_dialog_new(
            GTK_WINDOW(dialog), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO, _("preset `%s' already exists.\ndo you want to overwrite?"), name);
#ifdef GDK_WINDOWING_QUARTZ
        dt_osx_disallow_fullscreen(dlg_overwrite);
#endif

        gtk_window_set_title(GTK_WINDOW(dlg_overwrite), _("overwrite preset?"));

        const gint dlg_ret = gtk_dialog_run(GTK_DIALOG(dlg_overwrite));
        gtk_widget_destroy(dlg_overwrite);

        // if result is BUTTON_NO or ESCAPE keypress exit without destroying dialog, to permit other name
        if(dlg_ret == GTK_RESPONSE_YES)
        {
          // we remove the preset that will be overwrite
          dt_lib_presets_remove(name, g->operation, g->op_version);
        }
        else
          return;
      }
    }

    int format = 0;
    for(int k = 0; k < 5; k++)
      format += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[k])) * _gui_presets_format_flag[k];
    format ^= DT_PRESETS_FOR_NOT;

    // commit all the user input fields
    const dt_preset_conditions_t conditions
        = { .name = (gchar *)name,
            .description = (gchar *)gtk_entry_get_text(g->description),
            .model = (gchar *)gtk_entry_get_text(GTK_ENTRY(g->model)),
            .maker = (gchar *)gtk_entry_get_text(GTK_ENTRY(g->maker)),
            .lens = (gchar *)gtk_entry_get_text(GTK_ENTRY(g->lens)),
            .iso_min = gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->iso_min)),
            .iso_max = gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->iso_max)),
            .exposure_min = dt_gui_presets_exposure_value[dt_bauhaus_combobox_get(g->exposure_min)],
            .exposure_max = dt_gui_presets_exposure_value[dt_bauhaus_combobox_get(g->exposure_max)],
            .aperture_min = dt_gui_presets_aperture_value[dt_bauhaus_combobox_get(g->aperture_min)],
            .aperture_max = dt_gui_presets_aperture_value[dt_bauhaus_combobox_get(g->aperture_max)],
            .focal_length_min = gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->focal_length_min)),
            .focal_length_max = gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->focal_length_max)),
            .autoapply = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->autoapply)),
            .filter = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->filter)),
            .format = format };

    if(g->old_id >= 0)
    {
      // we update presets values
      dt_preset_repository_update_conditions(g->old_id, &conditions);
    }
    else if(g->iop)
    {
      // we create a new preset
      dt_preset_repository_insert_with_conditions(&conditions, g->operation, g->op_version,
                                                  g->iop->params, g->iop->params_size, g->iop->enabled,
                                                  g->iop->blend_params, sizeof(dt_develop_blend_params_t),
                                                  dt_develop_blend_version());
    }
    else
    {
      // we are in the lib case currently we set set all params to 0
      dt_preset_repository_insert_with_conditions(&conditions, g->operation, g->op_version,
                                                  NULL, 0, 0, NULL, 0, 0);
    }

    if(g->callback) ((void (*)(dt_gui_presets_edit_dialog_t *))g->callback)(g);
  }
  else if(response_id == GTK_RESPONSE_YES && g->old_id)
  {
    const gchar *name = gtk_entry_get_text(g->name);

    // ask for destination directory
    GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
          _("select directory"), GTK_WINDOW(dialog), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
          _("_select as output destination"), _("_cancel"));
    dt_conf_get_folder_to_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));

    // save if accepted
    if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
    {
      char *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
      dt_presets_save_to_file(g->old_id, name, filedir);
      dt_control_log(_("preset %s was successfully exported"), name);
      dt_free(filedir);
      dt_conf_set_folder_from_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));
    }

    g_object_unref(GTK_WIDGET(filechooser));
    return; // we don't close the window so other actions can be performed if needed
  }
  else if(response_id == GTK_RESPONSE_REJECT && g->old_id)
  {
    dt_gui_presets_confirm_and_delete(GTK_WIDGET(dialog), g->original_name, g->operation, g->old_id);

    if(g->callback) ((void (*)(dt_gui_presets_edit_dialog_t *))g->callback)(g);
  }

  GtkWindow *dialog_parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
  gtk_widget_destroy(GTK_WIDGET(dialog));
  dt_gui_refocus_parent(dialog_parent);
  dt_free(g->original_name);
  dt_free(g->module_name);
  dt_free(g->operation);
  dt_free(g);
}

void dt_gui_presets_confirm_and_delete(GtkWidget *parent_dialog, const char *name, const char *module_name, int rowid)
{
  if(IS_NULL_PTR(module_name)) return;

  // This means with want to remove the preset
  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(parent_dialog), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                             _("do you really want to delete the preset `%s'?"), name);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_window_set_title(GTK_WINDOW(dialog), _("delete preset?"));
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    // remove the preset from the database
    dt_preset_repository_delete_by_rowid_unprotected(rowid);
  }
  gtk_widget_destroy(dialog);
}

static void _check_buttons_activated(GtkCheckButton *button, dt_gui_presets_edit_dialog_t *g)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->autoapply))
     || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->filter)))
  {
    gtk_widget_set_visible(GTK_WIDGET(g->details), TRUE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->details), FALSE);
    gtk_widget_show_all(GTK_WIDGET(g->details));
    gtk_widget_set_no_show_all(GTK_WIDGET(g->details), TRUE);
  }
  else
    gtk_widget_set_visible(GTK_WIDGET(g->details), FALSE);
}

static void _presets_show_edit_dialog(dt_gui_presets_edit_dialog_t *g, gboolean allow_name_change,
                                      gboolean allow_desc_change, gboolean allow_remove)
{
  /* Create the widgets */
  char title[1024];
  snprintf(title, sizeof(title), _("edit `%s' for module `%s'"), g->original_name, g->module_name);
  GtkWidget *dialog = gtk_dialog_new_with_buttons
    (title, g->parent, GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
     _("_cancel"), GTK_RESPONSE_CANCEL, _("_export..."), GTK_RESPONSE_YES,
     _("delete"), GTK_RESPONSE_REJECT, _("_ok"), GTK_RESPONSE_OK, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING));
  gtk_container_add(content_area, GTK_WIDGET(box));

  g->name = GTK_ENTRY(gtk_entry_new());
  dt_accels_disconnect_on_text_input(GTK_WIDGET(g->name));
  gtk_entry_set_text(g->name, g->original_name);
  if(allow_name_change)
    gtk_entry_set_activates_default(g->name, TRUE);
  else
    gtk_widget_set_sensitive(GTK_WIDGET(g->name), FALSE);
  gtk_box_pack_start(box, GTK_WIDGET(g->name), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->name), _("name of the preset"));

  g->description = GTK_ENTRY(gtk_entry_new());
  dt_accels_disconnect_on_text_input(GTK_WIDGET(g->description));
  if(allow_desc_change)
    gtk_entry_set_activates_default(g->description, TRUE);
  else
    gtk_widget_set_sensitive(GTK_WIDGET(g->description), FALSE);
  gtk_box_pack_start(box, GTK_WIDGET(g->description), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->description), _("description or further information"));

  g->autoapply
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto apply this preset to matching images")));
  gtk_box_pack_start(box, GTK_WIDGET(g->autoapply), FALSE, FALSE, 0);
  g->filter
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("only show this preset for matching images")));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->filter), _("be very careful with this option. "
                                                           "this might be the last time you see your preset."));
  gtk_box_pack_start(box, GTK_WIDGET(g->filter), FALSE, FALSE, 0);
  if(IS_NULL_PTR(g->iop))
  {
    // lib usually don't support autoapply
    gtk_widget_set_no_show_all(GTK_WIDGET(g->autoapply), !dt_presets_module_can_autoapply(g->module_name));
    // for libs, we don't want the filtering option as it's not implemented...
    gtk_widget_set_no_show_all(GTK_WIDGET(g->filter), TRUE);
  }
  g_signal_connect(G_OBJECT(g->autoapply), "toggled", G_CALLBACK(_check_buttons_activated), g);
  g_signal_connect(G_OBJECT(g->filter), "toggled", G_CALLBACK(_check_buttons_activated), g);

  int line = 0;
  g->details = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(g->details), DT_GUI_BOX_SPACING);
  gtk_grid_set_column_spacing(GTK_GRID(g->details), DT_GUI_BOX_SPACING);
  gtk_box_pack_start(box, GTK_WIDGET(g->details), TRUE, TRUE, 0);

  GtkWidget *label = NULL;

  // model, maker, lens
  g->model = gtk_entry_new();
  dt_accels_disconnect_on_text_input(g->model);
  gtk_widget_set_hexpand(GTK_WIDGET(g->model), TRUE);
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->model, _("string to match model (use % as wildcard)"));
  label = gtk_label_new(_("model"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->model, label, GTK_POS_RIGHT, 2, 1);

  g->maker = gtk_entry_new();
  dt_accels_disconnect_on_text_input(g->maker);
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->maker, _("string to match maker (use % as wildcard)"));
  label = gtk_label_new(_("maker"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->maker, label, GTK_POS_RIGHT, 2, 1);

  g->lens = gtk_entry_new();
  dt_accels_disconnect_on_text_input(g->lens);
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->lens, _("string to match lens (use % as wildcard)"));
  label = gtk_label_new(_("lens"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->lens, label, GTK_POS_RIGHT, 2, 1);

  // iso
  label = gtk_label_new(_("ISO"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->iso_min = gtk_spin_button_new_with_range(0, FLT_MAX, 100);
  gtk_widget_set_tooltip_text(g->iso_min, _("minimum ISO value"));
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->iso_min), 0);
  g->iso_max = gtk_spin_button_new_with_range(0, FLT_MAX, 100);
  gtk_widget_set_tooltip_text(g->iso_max, _("maximum ISO value"));
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->iso_max), 0);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->iso_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->iso_max, g->iso_min, GTK_POS_RIGHT, 1, 1);

  // exposure
  label = gtk_label_new(_("exposure"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->exposure_min = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL));
  g->exposure_max = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL));
  gtk_widget_set_tooltip_text(g->exposure_min, _("minimum exposure time"));
  gtk_widget_set_tooltip_text(g->exposure_max, _("maximum exposure time"));
  for(int k = 0; k < dt_gui_presets_exposure_value_cnt; k++)
    dt_bauhaus_combobox_add(g->exposure_min, dt_gui_presets_exposure_value_str[k]);
  for(int k = 0; k < dt_gui_presets_exposure_value_cnt; k++)
    dt_bauhaus_combobox_add(g->exposure_max, dt_gui_presets_exposure_value_str[k]);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->exposure_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->exposure_max, g->exposure_min, GTK_POS_RIGHT, 1, 1);

  // aperture
  label = gtk_label_new(_("aperture"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->aperture_min = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL));
  g->aperture_max = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL));
  gtk_widget_set_tooltip_text(g->aperture_min, _("minimum aperture value"));
  gtk_widget_set_tooltip_text(g->aperture_max, _("maximum aperture value"));
  for(int k = 0; k < dt_gui_presets_aperture_value_cnt; k++)
    dt_bauhaus_combobox_add(g->aperture_min, dt_gui_presets_aperture_value_str[k]);
  for(int k = 0; k < dt_gui_presets_aperture_value_cnt; k++)
    dt_bauhaus_combobox_add(g->aperture_max, dt_gui_presets_aperture_value_str[k]);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->aperture_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->aperture_max, g->aperture_min, GTK_POS_RIGHT, 1, 1);

  // focal length
  label = gtk_label_new(_("focal length"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->focal_length_min = gtk_spin_button_new_with_range(0, 1000, 10);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->focal_length_min), 0);
  g->focal_length_max = gtk_spin_button_new_with_range(0, 1000, 10);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->focal_length_max), 0);
  gtk_widget_set_tooltip_text(g->focal_length_min, _("minimum focal length"));
  gtk_widget_set_tooltip_text(g->focal_length_max, _("maximum focal length"));
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->focal_length_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->focal_length_max, g->focal_length_min, GTK_POS_RIGHT, 1, 1);

  // raw/hdr/ldr/mono/color
  label = gtk_label_new(_("format"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line, 1, 1);
  gtk_widget_set_tooltip_text(label, _("select image types you want this preset to be available for"));

  for(int i = 0; i < 5; i++)
  {
    g->format_btn[i] = gtk_check_button_new_with_label(_(_gui_presets_format_value_str[i]));
    gtk_grid_attach(GTK_GRID(g->details), g->format_btn[i], 1, line + i, 2, 1);
  }

  gtk_widget_set_no_show_all(GTK_WIDGET(g->details), TRUE);

  dt_preset_conditions_t c = { 0 };
  int rowid = -1;
  if(dt_preset_repository_get_conditions(g->operation, g->op_version, g->original_name, &c, &rowid))
  {
    g->old_id = rowid;
    gtk_entry_set_text(GTK_ENTRY(g->description), c.description);
    gtk_entry_set_text(GTK_ENTRY(g->model), c.model);
    gtk_entry_set_text(GTK_ENTRY(g->maker), c.maker);
    gtk_entry_set_text(GTK_ENTRY(g->lens), c.lens);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->iso_min), c.iso_min);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->iso_max), c.iso_max);

    float val = c.exposure_min;
    int k = 0;
    for(; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_min, k);
    val = c.exposure_max;
    for(k = 0; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_max, k);
    val = c.aperture_min;
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_min, k);
    val = c.aperture_max;
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_max, k);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_min), c.focal_length_min);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_max), c.focal_length_max);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoapply), c.autoapply);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->filter), c.filter);
    const int format = c.format ^ DT_PRESETS_FOR_NOT;
    for(k = 0; k < 5; k++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->format_btn[k]), format & (_gui_presets_format_flag[k]));
    dt_preset_conditions_free(&c);
  }
  else
  {
    gtk_entry_set_text(GTK_ENTRY(g->description), "");
    gtk_entry_set_text(GTK_ENTRY(g->model), "%");
    gtk_entry_set_text(GTK_ENTRY(g->maker), "%");
    gtk_entry_set_text(GTK_ENTRY(g->lens), "%");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->iso_min), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->iso_max), FLT_MAX);

    float val = 0;
    int k = 0;
    for(; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_min, k);
    val = 100000000;
    for(k = 0; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_max, k);
    val = 0;
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_min, k);
    val = 100000000;
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_max, k);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_min), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_max), 1000);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoapply), 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->filter), 0);
    for(k = 0; k < 5; k++) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->format_btn[k]), TRUE);
  }

  // disable remove button if needed
  if(!allow_remove || g->old_id < 0)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);
    if(w) gtk_widget_set_sensitive(w, FALSE);
  }
  // disable export button if the preset is not already in the database
  if(g->old_id < 0)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    if(w) gtk_widget_set_sensitive(w, FALSE);
  }

  // put focus on cancel button if 2 first entries deactivated
  if(!allow_desc_change && !allow_name_change)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    if(w) gtk_widget_grab_focus(w);
  }

  g_signal_connect(dialog, "response", G_CALLBACK(_edit_preset_response), g);
  gtk_widget_show_all(dialog);
}

void dt_gui_presets_show_iop_edit_dialog(const char *name_in, dt_iop_module_t *module, GCallback final_callback,
                                         gpointer data, gboolean allow_name_change, gboolean allow_desc_change,
                                         gboolean allow_remove, GtkWindow *parent)
{
  dt_gui_presets_edit_dialog_t *g
      = (dt_gui_presets_edit_dialog_t *)g_malloc0(sizeof(dt_gui_presets_edit_dialog_t));
  g->old_id = -1;
  g->original_name = g_strdup(name_in);
  g->iop = module;
  g->operation = g_strdup(module->op);
  g->op_version = module->version();
  g->module_name = g_strdup(module->name());
  g->callback = final_callback;
  g->data = data;
  g->parent = parent;

  _presets_show_edit_dialog(g, allow_name_change, allow_desc_change, allow_remove);
}

void dt_gui_presets_show_edit_dialog(const char *name_in, const char *module_name, int rowid,
                                     GCallback final_callback, gpointer data, gboolean allow_name_change,
                                     gboolean allow_desc_change, gboolean allow_remove, GtkWindow *parent)
{
  gchar *operation = NULL;
  int op_version = 0;
  if(dt_preset_repository_get_identity(rowid, &operation, &op_version))
  {
    dt_gui_presets_edit_dialog_t *g
        = (dt_gui_presets_edit_dialog_t *)g_malloc0(sizeof(dt_gui_presets_edit_dialog_t));
    g->old_id = rowid;
    g->original_name = g_strdup(name_in);
    g->operation = operation; // ownership passes to the dialog
    g->op_version = op_version;
    g->module_name = g_strdup(module_name);
    g->callback = final_callback;
    g->data = data;
    g->parent = parent;

    _presets_show_edit_dialog(g, allow_name_change, allow_desc_change, allow_remove);
  }
}

static void _edit_preset(const char *name_in, dt_iop_module_t *module)
{
  gchar *name = NULL;
  if(IS_NULL_PTR(name_in))
  {
    int writeprotect = -1;
    name = _get_active_preset_name(module, &writeprotect);
    if(IS_NULL_PTR(name)) return;
    if(writeprotect)
    {
      dt_control_log(_("preset `%s' is write-protected! can't edit it!"), name);
      dt_free(name);
      return;
    }
  }
  else
    name = g_strdup(name_in);

  dt_gui_presets_show_iop_edit_dialog(name, module, (GCallback)_edit_preset_final_callback, NULL, TRUE, TRUE,
                                      FALSE, GTK_WINDOW(dt_gui_main_window()));
  dt_free(name);
}

static void _menuitem_edit_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  _edit_preset(NULL, module);
}

static void _menuitem_update_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  gchar *name = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");

  gint res = GTK_RESPONSE_YES;

  if(dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset"))
  {
    GtkWidget *window = dt_gui_main_window();
    GtkWidget *dialog
      = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_YES_NO, _("do you really want to update the preset `%s'?"), name);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), _("update preset?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    GtkWindow *dialog_parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
    gtk_widget_destroy(dialog);
    dt_gui_refocus_parent(dialog_parent);
  }

  if(res == GTK_RESPONSE_YES)
  {
    // commit all the module fields
    dt_preset_repository_update_iop_params(module->op, name, module->version(),
                                           module->params, module->params_size, module->enabled,
                                           module->blend_params, sizeof(dt_develop_blend_params_t),
                                           dt_develop_blend_version());
  }
}

static void _menuitem_new_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  // add new preset
  dt_lib_presets_remove(_("new preset"), module->op, module->version());

  // then show edit dialog
  _edit_preset(_("new preset"), module);
}

void dt_gui_presets_apply_preset(const gchar* name, dt_iop_module_t *module)
{
  dt_module_preset_t *p = dt_preset_repository_get_iop_preset(module->op, module->version(), name);

  if(p)
  {
    if(p->op_params && (p->op_params_size == module->params_size))
    {
      memcpy(module->params, p->op_params, p->op_params_size);
      module->enabled = p->enabled;
    }
    if(p->blendop_params && (p->blendop_version == dt_develop_blend_version())
       && (p->blendop_params_size == (int)sizeof(dt_develop_blend_params_t)))
    {
      dt_iop_commit_blend_params(module, p->blendop_params);
    }
    else if(p->blendop_params
            && dt_develop_blend_legacy_params(module, p->blendop_params, p->blendop_version,
                                              module->blend_params, dt_develop_blend_version(),
                                              p->blendop_params_size) == 0)
    {
      // do nothing
    }
    else
    {
      dt_iop_commit_blend_params(module, module->default_blendop_params);
    }

    if(!p->writeprotect) dt_gui_store_last_preset(name);
  }
  dt_module_preset_free(p);

  dt_iop_gui_update(module);
  dt_dev_add_history_item(dt_dev_get_global(), module, FALSE, TRUE);
  gtk_widget_queue_draw(module->widget);
}

static void _menuitem_pick_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  gchar *name = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");
  dt_gui_presets_apply_preset(name, module);
}

gboolean dt_gui_presets_autoapply_for_module(dt_iop_module_t *module)
{
  dt_image_t *image = &module->dev->image_storage;
  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);
  const char *workflow_preset = (has_matrix) ? _("scene-referred default") : "\t\n";

  int iformat = 0;
  if(dt_image_needs_rawprepare(image)) iformat |= FOR_RAW;
  else iformat |= FOR_LDR;
  if(dt_image_is_hdr(image)) iformat |= FOR_HDR;

  int excluded = 0;
  if(dt_image_monochrome_flags(image)) excluded |= FOR_NOT_MONO;
  else excluded |= FOR_NOT_COLOR;

  const dt_preset_match_t match = { .exif_model = image->exif_model,
                                    .exif_maker = image->exif_maker,
                                    .camera_alias = image->camera_alias,
                                    .camera_maker = image->camera_maker,
                                    .exif_lens = image->exif_lens,
                                    .iso = image->exif_iso,
                                    .exposure = image->exif_exposure,
                                    .aperture = image->exif_aperture,
                                    .focal_length = image->exif_focal_length,
                                    // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
                                    .format = iformat,
                                    .excluded = excluded };

  GList *names = dt_preset_repository_find_autoapply(module->op, module->version(),
                                                     &match, workflow_preset);

  gboolean applied = FALSE;
  for(GList *l = names; l; l = g_list_next(l))
  {
    dt_gui_presets_apply_preset((const char *)l->data, module);
    applied = TRUE;
  }
  g_list_free_full(names, g_free);

  return applied;
}

static gboolean _menuitem_button_released_preset(GtkMenuItem *menuitem, GdkEventButton *event,
                                                 dt_iop_module_t *module)
{
  if (event->button == 1 || (module->flags() & IOP_FLAGS_ONE_INSTANCE))
  {
    _menuitem_pick_preset(menuitem, module);
  }
  else if (event->button == 3)
  {
    dt_iop_module_t *new_module = dt_iop_gui_duplicate(module, FALSE);
    if(new_module) _menuitem_pick_preset(menuitem, new_module);
    dt_iop_gui_rename_module(new_module);
  }

  return FALSE;
}

#ifdef HAVE_OPENCL
static void _opencl_disable_callback(GtkButton *button, dt_iop_module_t *module)
{
  gboolean active = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(button));
  gchar *string = g_strdup_printf("/plugins/%s/opencl", module->op);
  dt_conf_set_bool(string, active);
  dt_free(string);
}

static void _cache_disable_callback(GtkButton *button, dt_iop_module_t *module)
{
  gboolean active = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(button));
  gchar *string = g_strdup_printf("/plugins/%s/cache", module->op);
  dt_conf_set_bool(string, active);
  dt_free(string);
}
#endif

// preset names containing '|' are split into a nested submenu hierarchy, the same way
// style names (src/libs/styles.c, src/gui/actions/styles.c) and tag names
// (src/libs/tagging.c) are grouped into a tree: every non-empty '|'-separated segment but
// the last becomes an intermediate submenu, the last segment is the leaf entry. `path` is
// the '|'-joined prefix built so far and is used as the cache key so a shared prefix reuses
// the same submenu instead of creating a duplicate.
static GtkWidget *_presets_get_submenu(GtkWidget *parent, GHashTable *submenus, const gchar *path,
                                      const gchar *label)
{
  GtkWidget *submenu = g_hash_table_lookup(submenus, path);
  if(submenu) return submenu;

  GtkWidget *item = gtk_menu_item_new_with_label(label);
  submenu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
  gtk_menu_shell_append(GTK_MENU_SHELL(parent), item);

  g_hash_table_insert(submenus, g_strdup(path), submenu);
  return submenu;
}

static void _gui_presets_popup_menu_show_internal(dt_dev_operation_t op, int32_t version,
                                                  dt_iop_params_t *params, int32_t params_size,
                                                  dt_develop_blend_params_t *bl_params, dt_iop_module_t *module,
                                                  const dt_image_t *image,
                                                  void (*pick_callback)(GtkMenuItem *, void *),
                                                  void *callback_data)
{
  GtkMenu *menu = dt_gui_get_global()->presets_popup_menu;
  if(menu) gtk_widget_destroy(GTK_WIDGET(menu));
  dt_gui_get_global()->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = dt_gui_get_global()->presets_popup_menu;
  const gboolean hide_default = dt_conf_get_bool("plugins/darkroom/hide_default_presets");
  const gboolean default_first = dt_conf_get_bool("modules/default_presets_first");

  GtkWidget *mi;
  int active_preset = -1, cnt = 0, writeprotect = 0; //, selected_default = 0;

  // order: get shipped defaults first
  dt_preset_match_t match = { 0 };
  gboolean have_match = FALSE;
  if(image)
  {
    // only matching if filter is on:
    int iformat = 0;
    if(dt_image_needs_rawprepare(image))
      iformat |= FOR_RAW;
    else
      iformat |= FOR_LDR;

    if(dt_image_is_hdr(image))
      iformat |= FOR_HDR;

    int excluded = 0;
    if(dt_image_monochrome_flags(image))
      excluded |= FOR_NOT_MONO;
    else
      excluded |= FOR_NOT_COLOR;

    match.exif_model = image->exif_model;
    match.exif_maker = image->exif_maker;
    match.camera_alias = image->camera_alias;
    match.camera_maker = image->camera_maker;
    match.exif_lens = image->exif_lens;
    match.iso = image->exif_iso;
    match.exposure = image->exif_exposure;
    match.aperture = image->exif_aperture;
    match.focal_length = image->exif_focal_length;
    match.format = iformat;
    match.excluded = excluded;
    have_match = TRUE;
  }

  GList *presets = dt_preset_repository_list_for_menu(op, have_match ? &match : NULL, default_first);
  // collect all presets for op from db
  gboolean found = 0;
  int last_wp = -1;
  GHashTable *submenus = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for(GList *l = presets; l; l = g_list_next(l))
  {
    const dt_module_preset_t *p = (const dt_module_preset_t *)l->data;
    const int chk_writeprotect = p->writeprotect;
    if(hide_default && chk_writeprotect)
    {
      //skip default module if set to hide them.
      continue;
    }
    if(last_wp == -1)
    {
      last_wp = chk_writeprotect;
    }
    else if(last_wp != chk_writeprotect)
    {
      last_wp = chk_writeprotect;
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }
    const void *op_params = p->op_params;
    const int32_t op_params_size = p->op_params_size;
    const void *blendop_params = p->blendop_params;
    const int32_t bl_params_size = p->blendop_params_size;
    const int32_t preset_version = p->op_version;
    const int32_t enabled = p->enabled;
    const int32_t isdisabled = (preset_version == version ? 0 : 1);
    const char *name = p->name;
    gboolean isdefault = FALSE;

    if(dt_gui_get_global()->last_preset && strcmp(dt_gui_get_global()->last_preset, name) == 0)
      found = TRUE;

    if(module
       && !memcmp(module->default_params, op_params,
                  MIN(op_params_size, module->params_size))
       && !memcmp(module->default_blendop_params, blendop_params,
                  MIN(bl_params_size, sizeof(dt_develop_blend_params_t))))
      isdefault = TRUE;

    gchar **split = g_strsplit(name, "|", -1);
    int leaf = -1;
    for(int i = 0; split[i]; i++)
      if(split[i][0] != '\0') leaf = i;

    GtkWidget *parent_menu = GTK_WIDGET(menu);
    if(leaf > 0)
    {
      GString *path = g_string_new(NULL);
      for(int i = 0; i < leaf; i++)
      {
        if(split[i][0] == '\0') continue;
        if(path->len > 0) g_string_append_c(path, '|');
        g_string_append(path, split[i]);
        parent_menu = _presets_get_submenu(parent_menu, submenus, path->str, split[i]);
      }
      g_string_free(path, TRUE);
    }
    const gchar *leaf_name = leaf >= 0 ? split[leaf] : name;

    gchar *label;
    if(isdefault)
      label = g_strdup_printf("%s %s", leaf_name, _("(default)"));
    else
      label = g_strdup(leaf_name);
    mi = gtk_menu_item_new_with_label(label);

    dt_free(label);
    g_strfreev(split);

    if(module
       && !memcmp(params, op_params, MIN(op_params_size, params_size))
       && !memcmp(bl_params, blendop_params, MIN(bl_params_size, sizeof(dt_develop_blend_params_t)))
       && module->enabled == enabled)
    {
      active_preset = cnt;
      writeprotect = p->writeprotect;
      dt_gui_add_class(mi, "menu-active");
    }

    if(isdisabled)
    {
      gtk_widget_set_sensitive(mi, 0);
      gtk_widget_set_tooltip_text(mi, _("disabled: wrong module version"));
    }
    else
    {
      g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(name), g_free);
      if(module)
      {
        g_signal_connect(G_OBJECT(mi), "button-release-event", G_CALLBACK(_menuitem_button_released_preset),
                         module);
      }
      else if(pick_callback)
        g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(pick_callback), callback_data);
      gtk_widget_set_tooltip_text(mi, p->description);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(parent_menu), mi);
    cnt++;
  }
  g_list_free_full(presets, dt_module_preset_free);
  g_hash_table_destroy(submenus);

  if(cnt > 0) gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  if(module)
  {
    if(active_preset >= 0 && !writeprotect)
    {
      mi = gtk_menu_item_new_with_label(_("edit this preset.."));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_edit_preset), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      mi = gtk_menu_item_new_with_label(_("delete this preset"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_delete_preset), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    else
    {
      mi = gtk_menu_item_new_with_label(_("store new preset.."));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_new_preset), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      if(dt_gui_get_global()->last_preset && found)
      {
        char *markup = g_markup_printf_escaped("%s <span weight='bold'>%s</span>", _("update preset"),
                                               dt_gui_get_global()->last_preset);
        mi = gtk_menu_item_new_with_label("");
        gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(mi))), markup);
        g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(dt_gui_get_global()->last_preset), g_free);
        g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_update_preset), module);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        dt_free(markup);
      }
    }
  }

  // and the parameters entry if needed
  if(module && (module->set_preferences))
  {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    if(module->set_preferences) module->set_preferences(GTK_MENU_SHELL(menu), module);
  }

#ifdef HAVE_OPENCL
  // OpenCL prefs
  if(module && module->process_cl)
  {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *item = gtk_check_menu_item_new_with_label(_("Use OpenCL (GPU computing)"));
    gtk_widget_set_tooltip_text(item, _("Run this module on GPU if possible.\n"
                                        "Disable if you face recurring issues on GPU with this module.\n"
                                        "Does not require a restart."));
    gchar *string = g_strdup_printf("/plugins/%s/opencl", module->op);

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), dt_conf_get_bool(string));
    dt_free(string);

    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_opencl_disable_callback), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_check_menu_item_new_with_label(_("Cache the GPU output"));
    gtk_widget_set_tooltip_text(item, _("Store the output of this module in cache when running on GPU.\n"
                                        "This may prevent some recomputations, at the cost of more memory I/O.\n"
                                        "The trade-off is worth it only for slow modules."));
    string = g_strdup_printf("/plugins/%s/cache", module->op);

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), dt_conf_get_bool(string));
    dt_free(string);

    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_cache_disable_callback), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  }
#endif
}

void dt_gui_presets_popup_menu_show_for_params(dt_dev_operation_t op, int32_t version, void *params,
                                               int32_t params_size, void *blendop_params,
                                               const dt_image_t *image,
                                               void (*pick_callback)(GtkMenuItem *, void *),
                                               void *callback_data)
{
  _gui_presets_popup_menu_show_internal(op, version, params, params_size, blendop_params, NULL, image,
                                        pick_callback, callback_data);
}

void dt_gui_presets_popup_menu_show_for_module(dt_iop_module_t *module)
{
  _gui_presets_popup_menu_show_internal(module->op, module->version(), module->params, module->params_size,
                                        module->blend_params, module, &module->dev->image_storage, NULL, NULL);
}

void dt_gui_presets_update_mml(const char *name, dt_dev_operation_t op, const int32_t version,
                               const char *maker, const char *model, const char *lens)
{
  dt_preset_repository_update_camera(op, version, name, maker, model, lens);
}

void dt_gui_presets_update_iso(const char *name, dt_dev_operation_t op, const int32_t version,
                               const float min, const float max)
{
  dt_preset_repository_update_range(op, version, name, DT_PRESET_RANGE_ISO, min, max);
}

void dt_gui_presets_update_av(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  dt_preset_repository_update_range(op, version, name, DT_PRESET_RANGE_APERTURE, min, max);
}

void dt_gui_presets_update_tv(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  dt_preset_repository_update_range(op, version, name, DT_PRESET_RANGE_EXPOSURE, min, max);
}

void dt_gui_presets_update_fl(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  dt_preset_repository_update_range(op, version, name, DT_PRESET_RANGE_FOCAL_LENGTH, min, max);
}

void dt_gui_presets_update_ldr(const char *name, dt_dev_operation_t op, const int32_t version,
                               const int ldrflag)
{
  dt_preset_repository_update_flag(op, version, name, DT_PRESET_FLAG_FORMAT, ldrflag);
}

void dt_gui_presets_update_autoapply(const char *name, dt_dev_operation_t op, const int32_t version,
                                     const int autoapply)
{
  dt_preset_repository_update_flag(op, version, name, DT_PRESET_FLAG_AUTOAPPLY, autoapply);
}

void dt_gui_presets_update_filter(const char *name, dt_dev_operation_t op, const int32_t version,
                                  const int filter)
{
  dt_preset_repository_update_flag(op, version, name, DT_PRESET_FLAG_FILTER, filter);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
