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


/* The two studio-capture questions that need a user, split out of common/folder_survey.c.
 *
 * They were the only reason a folder-monitoring state machine included gui/gtk.h -- and,
 * for the resume prompt, views/view.h as well, so a layer-1 module was driving a view
 * switch. Both dependencies leave with them.
 */

#include "gui/common/folder_survey_gui.h"

#include "common/conf.h"
#include "common/folder_survey.h"
#include "gui/gtk.h"
#include "views/view.h"

#include <glib/gi18n.h>

/* Asks about images sitting in the surveyed folder that the library does not have yet.
 * Returns TRUE to import them. The "delete originals" checkbox is only meaningful when
 * copy-on-import is on, and is persisted here because it is the checkbox's own state --
 * the backend has no opinion about it. */
static gboolean _confirm_pending_import(int new_files)
{
  GtkWindow *parent = GTK_WINDOW(dt_gui_main_window());
  GtkWidget *dialog = gtk_message_dialog_new(
      parent, GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
      ngettext("%d image in the surveyed folder is not in the library yet.\nImport it now?",
               "%d images in the surveyed folder are not in the library yet.\nImport them now?",
               new_files),
      new_files);

  GtkWidget *delete_check
      = gtk_check_button_new_with_label(_("Delete the originals after verifying the complete copies"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(delete_check),
                               dt_conf_get_bool("studio_capture/delete_source"));
  gtk_widget_set_sensitive(delete_check, dt_conf_get_bool("studio_capture/copy"));
  gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog))), delete_check,
                     FALSE, FALSE, DT_GUI_BOX_SPACING);
  gtk_widget_show_all(dialog);

  const int import_now = gtk_dialog_run(GTK_DIALOG(dialog));
  if(import_now == GTK_RESPONSE_YES && dt_conf_get_bool("studio_capture/copy"))
    dt_conf_set_bool("studio_capture/delete_source",
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(delete_check)));
  gtk_widget_destroy(dialog);
  dt_gui_refocus_parent(parent);

  return import_now == GTK_RESPONSE_YES;
}

void dt_folder_survey_gui_register_handlers()
{
  dt_folder_survey_set_confirm_import_handler(_confirm_pending_import);
}

gboolean dt_folder_survey_propose_resume()
{
  if(!dt_folder_survey_take_session_marker()) return G_SOURCE_REMOVE;

  char *folder = dt_conf_get_string("studio_capture/folder");
  GtkWindow *parent = GTK_WINDOW(dt_gui_main_window());

  GtkWidget *dialog = gtk_message_dialog_new(
      parent, GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
      _("A studio capture session was monitoring `%s` when Ansel was last closed.\n"
        "Resume the session?"),
      folder ? folder : "");
  const int resume = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  if(resume != GTK_RESPONSE_YES)
  {
    // Persist the cleared marker so the question is not asked again.
    dt_folder_survey_forget_session();
    dt_free(folder);
    return G_SOURCE_REMOVE;
  }

  dt_view_manager_switch(dt_view_manager_get_global(), "studio_capture");

  // dt_folder_survey_start() itself offers to import any images already
  // sitting in the folder, covering files that appeared while Ansel was
  // closed the same way it covers a plain session start.
  dt_folder_survey_start();
  dt_free(folder);
  return G_SOURCE_REMOVE;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
