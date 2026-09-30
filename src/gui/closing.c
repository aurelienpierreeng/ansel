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
// The details name a job's kind after the queue it was added to. The reserved worker, which only
// the darkroom uses, comes after the queues.
#define DT_CLOSING_KIND_DARKROOM DT_JOB_QUEUE_MAX
#define DT_CLOSING_KINDS (DT_JOB_QUEUE_MAX + 1)
// The rows of the details that can be unfolded: the two sections, and under the queued one, one row
// per kind, whose id is the kind itself. A job's row has none.
#define DT_CLOSING_ROW_RUNNING DT_CLOSING_KINDS
#define DT_CLOSING_ROW_QUEUED (DT_CLOSING_KINDS + 1)
#define DT_CLOSING_ROW_JOB (-1)

enum
{
  DT_CLOSING_COL_KIND,   // a job's kind, or a section's title
  DT_CLOSING_COL_DETAIL, // a job's description, or how many jobs a row holds
  DT_CLOSING_COL_WEIGHT, // bold for the sections
  DT_CLOSING_COL_ROW,    // which foldable row this is, to keep it unfolded across rebuilds
  DT_CLOSING_COLS
};

typedef struct dt_closing_t
{
  GMainLoop *loop;
  gint64 start;
  GtkWidget *window;
  GtkWidget *count;   // how many jobs are still running
  GtkWidget *names;   // what the ones that publish a progress say they are doing
  GtkWidget *details;  // the expander that lists the jobs, hidden while there are none
  GtkWidget *view;     // the tree inside it
  GtkTreeStore *store; // the tree's rows, owned by the view
  gchar *listed;       // what the tree shows, so that it is rebuilt only when that changes
} dt_closing_t;

typedef struct dt_closing_job_t
{
  int kind;
  gchar *description; // translated when the catalog knows it
} dt_closing_job_t;

typedef struct dt_closing_jobs_t
{
  GArray *running; // of dt_closing_job_t, in the order the scheduler reports them
  GArray *queued;  // likewise, for those that wait for a worker
} dt_closing_jobs_t;

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

  closing->store = gtk_tree_store_new(DT_CLOSING_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);
  closing->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(closing->store));
  g_object_unref(closing->store);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(closing->view), FALSE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(closing->view)), GTK_SELECTION_NONE);

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
      C_("closing jobs", "Type"), renderer, "text", DT_CLOSING_COL_KIND, "weight", DT_CLOSING_COL_WEIGHT, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(closing->view), column);

  renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  column = gtk_tree_view_column_new_with_attributes(C_("closing jobs", "Description"), renderer, "text",
                                                    DT_CLOSING_COL_DETAIL, "weight", DT_CLOSING_COL_WEIGHT, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(closing->view), column);

  // As tall as its rows up to a point, then it scrolls: a lighttable may queue hundreds of jobs.
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_ETCHED_IN);
  gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(420));
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(300));
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_widget_set_margin_top(scroll, DT_PIXEL_APPLY_DPI(6));
  gtk_container_add(GTK_CONTAINER(scroll), closing->view);
  gtk_container_add(GTK_CONTAINER(closing->details), scroll);
  // Shown by hand: the expander's no_show_all keeps show_all from reaching them.
  gtk_widget_show_all(scroll);

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
                                 const gboolean running, void *data)
{
  dt_closing_jobs_t *jobs = (dt_closing_jobs_t *)data;
  // Descriptions are written for the debug log, in English. Some are also the catalog's
  // strings, those the generic image jobs show on their progress bar.
  const dt_closing_job_t job = { .kind = reserved ? DT_CLOSING_KIND_DARKROOM : (int)queue,
                                 .description = g_strdup(_(description)) };
  g_array_append_val(running ? jobs->running : jobs->queued, job);
}

static gchar *_closing_count(const int count)
{
  return g_strdup_printf(ngettext("%d task", "%d tasks", count), count);
}

static void _closing_append(GtkTreeStore *store, GtkTreeIter *iter, GtkTreeIter *parent, const char *kind,
                            const char *detail, const int weight, const int row)
{
  gtk_tree_store_insert_with_values(store, iter, parent, -1, DT_CLOSING_COL_KIND, kind, DT_CLOSING_COL_DETAIL,
                                    detail, DT_CLOSING_COL_WEIGHT, weight, DT_CLOSING_COL_ROW, row, -1);
}

// Unfolds the row at iter if `unfold` and the mask has it, then says whether it is unfolded, as its
// bit in a mask of DT_CLOSING_COL_ROW. A job's row has no bit.
static guint _closing_foldable_row(const dt_closing_t *closing, GtkTreeIter *iter, const gboolean unfold,
                                   const guint mask)
{
  int row = DT_CLOSING_ROW_JOB;
  gtk_tree_model_get(GTK_TREE_MODEL(closing->store), iter, DT_CLOSING_COL_ROW, &row, -1);
  if(row < 0) return 0;

  const guint bit = 1u << row;
  GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(closing->store), iter);
  if(unfold && (mask & bit)) gtk_tree_view_expand_row(GTK_TREE_VIEW(closing->view), path, FALSE);
  const guint unfolded = gtk_tree_view_row_expanded(GTK_TREE_VIEW(closing->view), path) ? bit : 0;
  gtk_tree_path_free(path);
  return unfolded;
}

// Which rows are unfolded, as a mask of their DT_CLOSING_COL_ROW, after unfolding those of `mask`
// if `unfold`. Only the sections and the rows right under them have children.
static guint _closing_foldable_rows(const dt_closing_t *closing, const gboolean unfold, const guint mask)
{
  GtkTreeModel *model = GTK_TREE_MODEL(closing->store);
  guint unfolded = 0;
  GtkTreeIter section, child;
  for(gboolean s = gtk_tree_model_get_iter_first(model, &section); s; s = gtk_tree_model_iter_next(model, &section))
  {
    unfolded |= _closing_foldable_row(closing, &section, unfold, mask);
    for(gboolean c = gtk_tree_model_iter_children(model, &child, &section); c;
        c = gtk_tree_model_iter_next(model, &child))
      unfolded |= _closing_foldable_row(closing, &child, unfold, mask);
  }
  return unfolded;
}

// The running jobs, then the queued ones by kind, each kind folded on its jobs: a lighttable may
// queue hundreds of thumbnails. Jobs still queued at a quit never start, and the section says so.
static void _closing_details_update(dt_closing_t *closing)
{
  dt_closing_jobs_t jobs = { .running = g_array_new(FALSE, FALSE, sizeof(dt_closing_job_t)),
                             .queued = g_array_new(FALSE, FALSE, sizeof(dt_closing_job_t)) };
  g_array_set_clear_func(jobs.running, _closing_job_clear);
  g_array_set_clear_func(jobs.queued, _closing_job_clear);
  dt_control_jobs_foreach(dt_control_get_global(), _closing_collect_job, &jobs);

  GString *listed = g_string_new(NULL);
  for(guint i = 0; i < jobs.running->len; i++)
  {
    const dt_closing_job_t *job = &g_array_index(jobs.running, dt_closing_job_t, i);
    g_string_append_printf(listed, "r%d\t%s\n", job->kind, job->description);
  }
  for(guint i = 0; i < jobs.queued->len; i++)
  {
    const dt_closing_job_t *job = &g_array_index(jobs.queued, dt_closing_job_t, i);
    g_string_append_printf(listed, "q%d\t%s\n", job->kind, job->description);
  }

  if(g_strcmp0(listed->str, closing->listed) != 0)
  {
    // The first listing opens both sections and leaves the kinds folded; the next ones keep
    // what the user folded or unfolded.
    const guint unfolded = IS_NULL_PTR(closing->listed)
                               ? (1u << DT_CLOSING_ROW_RUNNING) | (1u << DT_CLOSING_ROW_QUEUED)
                               : _closing_foldable_rows(closing, FALSE, 0);
    gtk_tree_store_clear(closing->store);

    GtkTreeIter section, kind_row, row;
    if(jobs.running->len > 0)
    {
      gchar *count = _closing_count(jobs.running->len);
      _closing_append(closing->store, &section, NULL, _("Running"), count, PANGO_WEIGHT_BOLD,
                      DT_CLOSING_ROW_RUNNING);
      dt_free(count);
      for(guint i = 0; i < jobs.running->len; i++)
      {
        const dt_closing_job_t *job = &g_array_index(jobs.running, dt_closing_job_t, i);
        _closing_append(closing->store, &row, &section, _closing_kind_name(job->kind), job->description,
                        PANGO_WEIGHT_NORMAL, DT_CLOSING_ROW_JOB);
      }
    }
    if(jobs.queued->len > 0)
    {
      gchar *count = _closing_count(jobs.queued->len);
      _closing_append(closing->store, &section, NULL, _("Queued, will not run"), count, PANGO_WEIGHT_BOLD,
                      DT_CLOSING_ROW_QUEUED);
      dt_free(count);
      for(int kind = 0; kind < DT_CLOSING_KINDS; kind++)
      {
        int of_kind = 0;
        for(guint i = 0; i < jobs.queued->len; i++)
          if(g_array_index(jobs.queued, dt_closing_job_t, i).kind == kind) of_kind++;
        if(of_kind == 0) continue;

        count = _closing_count(of_kind);
        _closing_append(closing->store, &kind_row, &section, _closing_kind_name(kind), count, PANGO_WEIGHT_NORMAL,
                        kind);
        dt_free(count);
        for(guint i = 0; i < jobs.queued->len; i++)
        {
          const dt_closing_job_t *job = &g_array_index(jobs.queued, dt_closing_job_t, i);
          if(job->kind == kind)
            _closing_append(closing->store, &row, &kind_row, "", job->description, PANGO_WEIGHT_NORMAL,
                            DT_CLOSING_ROW_JOB);
        }
      }
    }
    _closing_foldable_rows(closing, TRUE, unfolded);
    gtk_widget_set_visible(closing->details, jobs.running->len + jobs.queued->len > 0);

    dt_free(closing->listed);
    closing->listed = g_string_free(listed, FALSE);
  }
  else
    g_string_free(listed, TRUE);

  g_array_free(jobs.running, TRUE);
  g_array_free(jobs.queued, TRUE);
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
  dt_free(closing.listed);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
