/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

#include "gui/closing.h"

#include "common/logging.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/progress.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "widgets/widget_settings.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h" // conditional-ok: its two calls below are under the same test
#endif

// A quit that is over within this long shows nothing: a window that flashes says less than none.
#define DT_CLOSING_NOTICE_DELAY (G_USEC_PER_SEC)
// How often the workers are counted, and the window brought up to date, in milliseconds.
#define DT_CLOSING_POLL_INTERVAL 100

typedef struct dt_closing_t
{
  GMainLoop *loop;
  gint64 start;
  GtkWidget *window;
  GtkWidget *count; // how many jobs are still running
  GtkWidget *names; // what the ones that publish a progress say they are doing
} dt_closing_t;

static void _closing_window_new(dt_closing_t *closing)
{
  closing->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  // Like every other window of ours: it must not open as a full-screen space of its own, and
  // it must show over the one the main window may have left.
  dt_osx_disallow_fullscreen(closing->window);
#endif
  gtk_window_set_icon_name(GTK_WINDOW(closing->window), "ansel");
  gtk_window_set_title(GTK_WINDOW(closing->window), _("closing Ansel..."));
  gtk_window_set_position(GTK_WINDOW(closing->window), GTK_WIN_POS_CENTER);
  gtk_window_set_resizable(GTK_WINDOW(closing->window), FALSE);
  // Closing this window would stop nothing, so it offers no button to. The grab of
  // dt_gui_closing_wait() is what discards a delete-event sent some other way.
  gtk_window_set_deletable(GTK_WINDOW(closing->window), FALSE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(12));
  gtk_container_set_border_width(GTK_CONTAINER(box), DT_PIXEL_APPLY_DPI(16));
  gtk_container_add(GTK_CONTAINER(closing->window), box);

  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_set_valign(spinner, GTK_ALIGN_START);
  gtk_spinner_start(GTK_SPINNER(spinner));
  gtk_box_pack_start(GTK_BOX(box), spinner, FALSE, FALSE, 0);

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(8));
  gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);

  GtkWidget *title = gtk_label_new(NULL);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", _("Ansel is finishing its work before closing"));
  gtk_label_set_markup(GTK_LABEL(title), markup);
  dt_free(markup);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0);
  gtk_box_pack_start(GTK_BOX(text), title, FALSE, FALSE, 0);

  closing->count = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(closing->count), 0.0);
  gtk_box_pack_start(GTK_BOX(text), closing->count, FALSE, FALSE, 0);

  closing->names = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(closing->names), 0.0);
  gtk_widget_set_no_show_all(closing->names, TRUE);
  gtk_box_pack_start(GTK_BOX(text), closing->names, FALSE, FALSE, 0);

  GtkWidget *hint = gtk_label_new(_("This window closes by itself as soon as it is done."));
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
  gtk_box_pack_start(GTK_BOX(text), hint, FALSE, FALSE, 0);

  gtk_widget_show_all(closing->window);

  // A quit does not always come from the application in front: the Dock's Quit, or Cmd+Q
  // through the application switcher, leave another one active, and a window opened by an
  // application in the background opens behind the windows of the one that is not.
  gtk_window_present(GTK_WINDOW(closing->window));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_focus_window();
#endif
}

static void _closing_append_name(const gchar *message, void *data)
{
  GString *names = (GString *)data;
  if(names->len > 0) g_string_append_c(names, '\n');
  g_string_append(names, message);
}

static gboolean _closing_poll(gpointer user_data)
{
  dt_closing_t *closing = (dt_closing_t *)user_data;
  const int32_t alive = dt_control_workers_alive();
  if(alive == 0)
  {
    g_main_loop_quit(closing->loop);
    return G_SOURCE_REMOVE;
  }

  if(IS_NULL_PTR(closing->window))
  {
    if(g_get_monotonic_time() - closing->start < DT_CLOSING_NOTICE_DELAY) return G_SOURCE_CONTINUE;
    _closing_window_new(closing);
  }

  gchar *count = g_strdup_printf(ngettext("%d task is still running", "%d tasks are still running", alive), alive);
  gtk_label_set_text(GTK_LABEL(closing->count), count);
  dt_free(count);

  GString *names = g_string_new(NULL);
  dt_control_progress_foreach(dt_control_get_global(), _closing_append_name, names);
  gtk_label_set_text(GTK_LABEL(closing->names), names->str);
  gtk_widget_set_visible(closing->names, names->len > 0);
  g_string_free(names, TRUE);

  return G_SOURCE_CONTINUE;
}

void dt_gui_closing_wait(void)
{
  if(dt_control_workers_alive() == 0) return;

  // The main window is hidden and its view left, but the windows it may have left on screen are
  // not, and nothing behind them is in a state to answer. The grab takes every input event
  // away from them, close buttons included, for as long as this loop turns. Its widget is
  // realised, since GTK delivers events to realised widgets only, but never shown: mapped, it
  // would be a window on screen -- on macOS, an NSWindow of its own.
  GtkWidget *grab = gtk_invisible_new();
  gtk_widget_realize(grab);
  gtk_grab_add(grab);

  dt_closing_t closing = { .loop = g_main_loop_new(NULL, FALSE), .start = g_get_monotonic_time() };
  g_timeout_add(DT_CLOSING_POLL_INTERVAL, _closing_poll, &closing);
  g_main_loop_run(closing.loop);
  g_main_loop_unref(closing.loop);

  gtk_grab_remove(grab);
  gtk_widget_destroy(grab);

  dt_print(DT_DEBUG_CONTROL, "[closing] waited %.2f s for the running jobs\n",
           (g_get_monotonic_time() - closing.start) / (double)G_USEC_PER_SEC);

  // Off the screen at the drain of the main context that dt_cleanup() does right after
  // dt_control_shutdown(), whose joins no longer wait for anything.
  if(!IS_NULL_PTR(closing.window)) gtk_widget_destroy(closing.window);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
