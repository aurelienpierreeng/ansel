/*
 *    This file is part of darktable,
 *    Copyright (C) 2016 johannes hanika.
 *    Copyright (C) 2016, 2020 Tobias Ellinghaus.
 *    Copyright (C) 2020 Pascal Obry.
 *    Copyright (C) 2021 Sakari Kapanen.
 *    Copyright (C) 2022 Martin Bařinka.
 *    
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *    
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "gui/common/database_gui.h"

#include "common/database.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "widgets/dialog.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <stdio.h>

gboolean dt_database_show_error(struct dt_database_t *db)
{
  dt_database_error_t err = { 0 };
  dt_database_take_error(db, &err);

  gboolean error = TRUE;

  if(!err.lock_acquired)
  {
    char lck_pathname[1024];
    snprintf(lck_pathname, sizeof(lck_pathname), "%s.lock", err.dbfilename);
    char *lck_dirname = g_strdup(lck_pathname);
    char *slash_pos = g_strrstr(lck_dirname, "/");
    if(!IS_NULL_PTR(slash_pos)) *slash_pos = '\0';
    // clang-format off
    char *label_text = g_markup_printf_escaped(
        _("\n"
          "  Sorry, Ansel could not be started because the database is locked.\n"
          "\n"
          "  How to solve this problem?\n"
          "\n"
          "  1 - If another Ansel instance is already running, \n"
          "      click \"Quit\" and either use that instance or close it before trying to start Ansel again. \n"
          "      (process ID <i><b>%d</b></i> created the database lock files)\n"
          "\n"
          "  2 - If you cannot find any running instance of Ansel, try restarting your session or your computer. \n"
          "      This will close all running programs and should release any database locks. \n"
          "\n"
            "  3 - If you have already tried the above steps, or you are certain that no other instances of Ansel are running, \n"
            "      this likely means the previous instance ended unexpectedly. \n"
            "      Click the \"Delete database lock files\" button to remove <i>data.db.lock</i> and <i>library.db.lock</i>. \n"
            "      Ansel will then attempt to load the database again. \n"
          "\n\n"
          "      <i><u>Caution!</u> Do not delete these files without first undertaking the above checks, \n"
          "      otherwise you risk generating serious inconsistencies in your database.</i>\n"),
      err.other_pid);
    // clang-format on

    const int choice = dt_gui_show_standalone_three_choice_dialog(_("Error starting Ansel"), label_text,
                                        _("Quit"), _("Retry"), _("Delete database lock files and try again"));

    if(choice == 1)
    {
      // Just try to acquire the lock again: useful once the other instance that held it has
      // since closed. dt_database_show_error() returning FALSE makes the caller's init loop
      // (darktable.c) re-run dt_database_init() without touching any lock file.
      error = FALSE;
    }
    else if(choice == 2)
    {
      gboolean really_delete_lockfiles =
        dt_gui_show_standalone_yes_no_dialog
        (_("Confirmation"),
         _("\n<u>Caution!</u> Are you sure you want to delete the database lock files?\n"
          "This action should only be performed if you are certain no other Ansel instances are running.\n"), _("Quit"), _("Yes"));
      if(really_delete_lockfiles)
      {
        // deleting files is the backend's job; this module only obtained consent
        const int status = dt_database_delete_lock_files(err.dbfilename);

        if(status==0)
        {
          dt_gui_show_standalone_yes_no_dialog(_("Done"),
                                        _("\nThe database lock files have been deleted successfully.\n"),
                                        _("Continue"), NULL);
          error = FALSE;
        }

        else
        {
          // The dialog copies the markup rather than taking ownership, so this is ours to free.
          gchar *err_text = g_markup_printf_escaped(
              _("\nAt least one lock file could not be removed.\n"
                "You may try to manually delete the files <i>data.db.lock</i> and <i>library.db.lock</i>\n"
                "in folder <a href=\"file:///%s\">%s</a>.\n"), lck_dirname, lck_dirname);
          dt_gui_show_standalone_yes_no_dialog(_("Error"), err_text, _("Quit"), NULL);
          dt_free(err_text);
        }
      }
    }

    dt_free(lck_dirname);
    dt_free(label_text);
  }

  dt_database_error_free(&err);
  return error;
}

/* The three prompts dt_database_init() puts, now that it only states them.
 *
 * They were built inline in common/database.c with no has_gui guard at all, so a headless
 * run reached gtk_dialog_new_with_buttons() on a GTK that ansel-cli never initialises.
 * Registration is what gates them now, and darktable.c only registers when there is a GUI.
 */
static dt_database_response_t _database_prompt(const dt_database_prompt_t prompt, const char *dbfilename,
                                               const char *quick_check, const gboolean snapshot_available)
{
  const GtkDialogFlags dflags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
  GtkWidget *dialog = NULL;
  char *label_text = NULL;

  if(prompt == DT_DATABASE_PROMPT_READONLY)
  {
    dialog = gtk_dialog_new_with_buttons(_("Ansel - Database is read-only"), NULL, dflags,
                                         _("Close Ansel"), GTK_RESPONSE_CLOSE, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
    label_text = g_markup_printf_escaped(_("<span weight='bold'>Ansel library database is read-only</span>\n\n"
                                           "This happens if you don't have permissions to write on the filesystem\n"
                                           "or if you have restored a write-protected backup snapshot.\n\n"
                                           "Please change the filesystem access permissions for:\n\n"
                                           "\t<span style='italic'>%s</span>"),
                                         dbfilename);
  }
  else
  {
    const char *label_options;
    if(snapshot_available)
    {
      dialog = gtk_dialog_new_with_buttons(_("ansel - error opening database"), NULL, dflags,
                                           _("close Ansel"), GTK_RESPONSE_CLOSE,
                                           _("attempt restore"), GTK_RESPONSE_ACCEPT,
                                           _("delete database"), GTK_RESPONSE_REJECT, NULL);
      gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
      label_options = _("do you want to close Ansel now to manually restore\n"
                        "the database from a backup, attempt an automatic restore\n"
                        "from the most recent snapshot or delete the corrupted database\n"
                        "and start with a new one?");
    }
    else
    {
      dialog = gtk_dialog_new_with_buttons(_("ansel - error opening database"), NULL, dflags,
                                           _("close Ansel"), GTK_RESPONSE_CLOSE,
                                           _("delete database"), GTK_RESPONSE_REJECT, NULL);
      gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
      label_options = _("do you want to close Ansel now to manually restore\n"
                        "the database from a backup or delete the corrupted database\n"
                        "and start with a new one?");
    }

    // quick_check is sqlite output, i.e. arbitrary text landing in a markup label. Escaping
    // is the handler's job because only the handler knows it is markup at all.
    label_text = g_markup_printf_escaped(_("an error has occurred while trying to open the database from\n"
                                           "\n"
                                           "<span style='italic'>%s</span>\n"
                                           "\n"
                                           "it seems that the database is corrupted.\n"
                                           "%s%s"),
                                         dbfilename, IS_NULL_PTR(quick_check) ? "" : quick_check, label_options);
  }

  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), label_text);
  dt_free(label_text);
  gtk_container_add(GTK_CONTAINER(content_area), label);
  gtk_widget_show_all(content_area);

  const int resp = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  switch(resp)
  {
    case GTK_RESPONSE_ACCEPT:
      return DT_DATABASE_RESPONSE_RESTORE;
    case GTK_RESPONSE_REJECT:
      return DT_DATABASE_RESPONSE_DELETE;
    default:
      // Covers the window being closed outright, which must not be read as consent to
      // delete anything.
      return DT_DATABASE_RESPONSE_CLOSE;
  }
}

void dt_database_gui_register_handlers(void)
{
  dt_database_set_prompt_handler(_database_prompt);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
