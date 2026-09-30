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
#include "widgets/widget_style.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h" // conditional-ok: its two calls below are under the same test
#endif

// A quit that is over within this long shows nothing: a window that flashes says less than none.
#define DT_CLOSING_NOTICE_DELAY (G_USEC_PER_SEC)
// How often the workers are counted, and the window brought up to date, in milliseconds.
#define DT_CLOSING_POLL_INTERVAL 100
// A job's kind, as the details name it, is the queue it was added to. The reserved worker, which
// only the darkroom uses, comes after the queues.
#define DT_CLOSING_KIND_DARKROOM DT_JOB_QUEUE_MAX

enum
{
  DT_CLOSING_COL_KIND,        // the job's kind
  DT_CLOSING_COL_DESCRIPTION, // the description it was created with
  DT_CLOSING_COLS
};

typedef struct dt_closing_t
{
  GMainLoop *loop;
  gint64 start;
  GtkWidget *window;
  GtkWidget *count;    // how many jobs are still running
  GtkWidget *names;    // what the ones that publish a progress say they are doing
  GtkWidget *details;  // the expander that lists the running jobs, hidden while there is nothing in it
  GtkListStore *store; // its rows, owned by the view inside it
  gchar *listed;       // what the list shows, so that it is rebuilt only when that changes
  GtkWidget *dropped;  // under the list, how many queued jobs the quit drops
  int32_t queued;      // what it says, -1 before it says anything
} dt_closing_t;

typedef struct dt_closing_job_t
{
  int kind;
  gchar *description; // translated when the catalog knows it
} dt_closing_job_t;

static gboolean _closing_refuse_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  return TRUE;
}

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
  // Closing this window would stop nothing, so it offers no button to, and refuses a
  // delete-event sent some other way (Alt+F4, a task bar).
  gtk_window_set_deletable(GTK_WINDOW(closing->window), FALSE);
  g_signal_connect(closing->window, "delete-event", G_CALLBACK(_closing_refuse_delete), NULL);
  // A window group of its own. The grab of dt_gui_closing_wait() is held in the default group,
  // the one every other window of ours is in; in there, it would take this window's clicks too,
  // and the details could not be unfolded.
  GtkWindowGroup *group = gtk_window_group_new();
  gtk_window_group_add_window(group, GTK_WINDOW(closing->window));
  g_object_unref(group);

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

  closing->details = gtk_expander_new(_("Details"));
  gtk_expander_set_resize_toplevel(GTK_EXPANDER(closing->details), TRUE);
  gtk_widget_set_no_show_all(closing->details, TRUE);
  gtk_box_pack_start(GTK_BOX(text), closing->details, FALSE, FALSE, 0);

  closing->store = gtk_list_store_new(DT_CLOSING_COLS, G_TYPE_STRING, G_TYPE_STRING);
  GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(closing->store));
  g_object_unref(closing->store);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), FALSE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), GTK_SELECTION_NONE);

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(C_("closing jobs", "Type"), renderer,
                                                                       "text", DT_CLOSING_COL_KIND, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

  renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  column = gtk_tree_view_column_new_with_attributes(C_("closing jobs", "Description"), renderer, "text",
                                                    DT_CLOSING_COL_DESCRIPTION, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

  // As tall as its rows up to a point, then it scrolls: there is one row per worker at most. The
  // tree view is transparent, and the recessed frame of our other lists puts it on a dark ground.
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  dt_gui_add_class(scroll, "dt_recessed_scroll");
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(420));
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(300));
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_container_add(GTK_CONTAINER(scroll), view);

  closing->dropped = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(closing->dropped), 0.0);
  gtk_widget_set_no_show_all(closing->dropped, TRUE);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(6));
  gtk_widget_set_margin_top(content, DT_PIXEL_APPLY_DPI(6));
  gtk_box_pack_start(GTK_BOX(content), scroll, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), closing->dropped, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(closing->details), content);
  // Shown by hand: the expander's no_show_all keeps show_all from reaching them. The count of
  // dropped jobs keeps its own, and shows only when there are some.
  gtk_widget_show_all(content);

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

static const char *_closing_kind_name(const int kind)
{
  switch(kind)
  {
    case DT_JOB_QUEUE_USER_FG:
      return C_("job type", "Image operation");
    case DT_JOB_QUEUE_SYSTEM_FG:
      return C_("job type", "Thumbnail");
    case DT_JOB_QUEUE_USER_BG:
      return C_("job type", "Background task");
    case DT_JOB_QUEUE_USER_EXPORT: // prints share this queue: their description says so
      return C_("job type", "Export");
    case DT_JOB_QUEUE_SYSTEM_BG:
      return C_("job type", "Maintenance");
    default:
      return C_("job type", "Darkroom rendering");
  }
}

static void _closing_job_clear(gpointer data)
{
  dt_closing_job_t *job = (dt_closing_job_t *)data;
  dt_free(job->description);
}

static void _closing_collect_job(const char *description, const dt_job_queue_t queue, const gboolean reserved,
                                 void *data)
{
  GArray *running = (GArray *)data;
  // Descriptions are written for the debug log, in English. Some are also the catalog's
  // strings, those the generic image jobs show on their progress bar.
  const dt_closing_job_t job = { .kind = reserved ? DT_CLOSING_KIND_DARKROOM : (int)queue,
                                 .description = g_strdup(_(description)) };
  g_array_append_val(running, job);
}

// One row per running job. The queued ones are only counted, under the list: a quit drops them,
// unrun.
static void _closing_details_update(dt_closing_t *closing)
{
  GArray *running = g_array_new(FALSE, FALSE, sizeof(dt_closing_job_t));
  g_array_set_clear_func(running, _closing_job_clear);
  dt_control_running_jobs_foreach(dt_control_get_global(), _closing_collect_job, running);

  GString *listed = g_string_new(NULL);
  for(guint i = 0; i < running->len; i++)
  {
    const dt_closing_job_t *job = &g_array_index(running, dt_closing_job_t, i);
    g_string_append_printf(listed, "%d\t%s\n", job->kind, job->description);
  }

  if(g_strcmp0(listed->str, closing->listed) != 0)
  {
    gtk_list_store_clear(closing->store);
    for(guint i = 0; i < running->len; i++)
    {
      const dt_closing_job_t *job = &g_array_index(running, dt_closing_job_t, i);
      gtk_list_store_insert_with_values(closing->store, NULL, -1, DT_CLOSING_COL_KIND, _closing_kind_name(job->kind),
                                        DT_CLOSING_COL_DESCRIPTION, job->description, -1);
    }
    dt_free(closing->listed);
    closing->listed = g_string_free(listed, FALSE);
  }
  else
    g_string_free(listed, TRUE);

  const int32_t queued = dt_control_queued_jobs_count(dt_control_get_global());
  if(queued != closing->queued)
  {
    gchar *dropped = g_strdup_printf(ngettext("%d other task, not started, is dropped",
                                              "%d other tasks, not started, are dropped", queued),
                                     queued);
    gtk_label_set_text(GTK_LABEL(closing->dropped), dropped);
    dt_free(dropped);
    gtk_widget_set_visible(closing->dropped, queued > 0);
    closing->queued = queued;
  }

  gtk_widget_set_visible(closing->details, running->len > 0 || queued > 0);
  g_array_free(running, TRUE);
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

  _closing_details_update(closing);

  return G_SOURCE_CONTINUE;
}

void dt_gui_closing_wait(void)
{
  if(dt_control_workers_alive() == 0) return;

  // The main window is hidden and its view left, but the windows it may have left on screen are
  // not, and nothing behind them is in a state to answer. The grab takes every input event
  // away from them, close buttons included, for as long as this loop turns. It holds the
  // default window group, theirs; the closing window has one of its own, and escapes it. Its
  // widget is realised, since GTK delivers events to realised widgets only, but never shown:
  // mapped, it would be a window on screen -- on macOS, an NSWindow of its own.
  GtkWidget *grab = gtk_invisible_new();
  gtk_widget_realize(grab);
  gtk_grab_add(grab);

  dt_closing_t closing = { .loop = g_main_loop_new(NULL, FALSE), .start = g_get_monotonic_time(), .queued = -1 };
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
  dt_free(closing.listed);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
