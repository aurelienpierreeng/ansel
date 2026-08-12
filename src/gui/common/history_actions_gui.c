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

/* The three history actions that need to ask the user something.
 *
 * They were in common/history_actions.c, which is otherwise pure backend -- and were the
 * only reason it included gui/hist_dialog.h, gui/gtk.h and gui/actions/menu.h. Nothing
 * about them is shared with the batch machinery there: two open the module-picker dialog
 * and store its answer in the copy/paste proxy, the third is a GtkAccelGroup accelerator.
 *
 * Every caller already lives at or above this layer (gui/actions/edit.c, libs/history.c),
 * so this is a relocation, not an inversion: no handler slot is needed.
 */

#include "gui/common/history_actions_gui.h"

#include "common/act_on.h"
#include "common/conf.h"
#include "common/history_actions.h"
#include "system/macros.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/dev_history.h"
#include "gui/actions/menu.h"
#include "gui/application.h"
#include "gui/hist_dialog.h"
#include "views/view.h"

#include <glib/gi18n.h>
#include "widgets/dialog.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

gboolean dt_history_copy_parts(int32_t imgid)
{
  if(dt_history_copy(imgid))
  {
    // run dialog, it will insert into selops the selected moduel

    if(dt_gui_hist_dialog_new(dt_history_copy_paste_get(), imgid, TRUE) == GTK_RESPONSE_CANCEL)
      return FALSE;
    return TRUE;
  }
  else
    return FALSE;
}


gboolean dt_history_paste_parts_prepare(void)
{
  dt_history_copy_item_t *copy_paste = dt_history_copy_paste_get();
  if(copy_paste->copied_imageid <= 0) return FALSE;

  // we launch the dialog
  const int res = dt_gui_hist_dialog_new(copy_paste, copy_paste->copied_imageid, FALSE);

  if(res != GTK_RESPONSE_OK)
  {
    return FALSE;
  }

  return TRUE;
}


gboolean delete_history_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(!has_active_images()) return FALSE;

  GList *imgs = dt_act_on_get_images();
  if(IS_NULL_PTR(imgs)) return FALSE;

  if(dt_conf_get_bool("ask_before_discard"))
  {
    const int img_count = g_list_length(imgs);
    const GtkWidget *win = dt_gui_main_window();
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("Do you really want to clear history of %d image?",
                 "Do you really want to clear history of %d images?", img_count),
        img_count);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("Delete image's history?", "Delete images' history?", img_count));

    GtkWidget *message_area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
    GtkWidget *ask_check = gtk_check_button_new_with_label(_("Always ask"));
    gtk_widget_set_tooltip_text(ask_check,
        _("when unchecked, history will be deleted silently from now on without this confirmation.\n"
          "you can turn it back on from preferences."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ask_check), TRUE);
    gtk_box_pack_start(GTK_BOX(message_area), ask_check, FALSE, FALSE, 6);
    gtk_widget_show(ask_check);

    const gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    dt_conf_set_bool("ask_before_discard", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ask_check)));
    gtk_widget_destroy(dialog);
    dt_gui_refocus_parent(GTK_WINDOW(win));
    if(res != GTK_RESPONSE_YES)
    {
      g_list_free(imgs);
      return TRUE;
    }
  }

  gboolean is_darkroom_image_in_list = dt_dev_history_is_image_in_dev(imgs);

  dt_develop_t *dev = dt_dev_get_global();

  if(is_darkroom_image_in_list)
  {
    dt_dev_undo_start_record(dev);
  }

  dt_history_delete_on_list(imgs, TRUE);

  if(is_darkroom_image_in_list)
  {
    dt_dev_undo_end_record(dev);
    dt_apply_dev_history_update(dev);
  }

  dt_control_queue_redraw_center();
  g_list_free(imgs);
  imgs = NULL;
  return TRUE;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
