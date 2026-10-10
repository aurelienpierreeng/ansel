/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "gui/auto_group.h"
#include "common/auto_group.h"
#include "common/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "control/user_message.h"
#include "gui/application.h"
#include "widgets/bauhaus.h"
#include "widgets/widget_settings.h"

#define AUTO_GROUP_INTERVAL_CONF "plugins/lighttable/auto_group/interval"

/** @brief Main-thread state; one worker at a time borrows a ref-counted immutable snapshot.
 * @details GTask retains the dialog until completion. Destroy cancels its timeout and disconnects
 * notifications; the closed flag prevents the retained object from accessing destroyed children.
 * Generation changes discard obsolete results without queuing unbounded worker jobs.
 */
typedef struct dt_auto_group_dialog_t
{
  GtkWidget *interval;
  GtkWidget *duration;
  GtkWidget *summary;
  dt_auto_group_snapshot_t *snapshot;
  guint preview_source;
  guint64 generation;
  gboolean snapshot_dirty;
  gboolean preview_running;
  gboolean closed;
} dt_auto_group_dialog_t;

typedef struct dt_auto_group_preview_t
{
  dt_auto_group_snapshot_t *snapshot;
  guint64 generation;
  int interval;
  size_t groups;
  size_t ungrouped;
  size_t skipped;
} dt_auto_group_preview_t;

static gboolean _auto_group_preview_start(gpointer user_data);

/** @brief Release the rows when the last dialog/worker reference to a snapshot is dropped. */
static void _auto_group_snapshot_free(gpointer user_data)
{
  const dt_auto_group_snapshot_t *snapshot = user_data;
  dt_free(snapshot->images);
}

static void _auto_group_preview_free(gpointer user_data)
{
  dt_auto_group_preview_t *preview = user_data;
  g_atomic_rc_box_release_full(preview->snapshot, _auto_group_snapshot_free);
  dt_free(preview);
}

static void _auto_group_preview_worker(GTask *task, gpointer source_object G_GNUC_UNUSED, gpointer task_data,
                                       GCancellable *cancellable G_GNUC_UNUSED)
{
  dt_auto_group_preview_t *preview = task_data;
  dt_auto_group_plan_t plan = { 0 };
  const dt_auto_group_status_t status = dt_auto_group_plan(preview->snapshot, preview->interval, &plan);
  preview->groups = plan.groups;
  preview->ungrouped = plan.ungrouped;
  preview->skipped = plan.skipped;
  dt_auto_group_plan_cleanup(&plan);
  g_task_return_int(task, status);
}

static void _auto_group_preview_ready(GObject *source, GAsyncResult *result, gpointer user_data G_GNUC_UNUSED)
{
  dt_auto_group_dialog_t *data = g_object_get_data(source, "auto-group-dialog");
  const dt_auto_group_preview_t *preview = g_task_get_task_data(G_TASK(result));
  const dt_auto_group_status_t status = g_task_propagate_int(G_TASK(result), NULL);
  data->preview_running = FALSE;
  if(data->closed) return;
  if(preview->generation != data->generation)
  {
    if(!data->preview_source)
      data->preview_source = g_timeout_add(150, _auto_group_preview_start, source);
    return;
  }
  if(status != DT_AUTO_GROUP_STATUS_OK && status != DT_AUTO_GROUP_STATUS_EMPTY)
  {
    gtk_label_set_text(GTK_LABEL(data->summary), _("Preview unavailable."));
    return;
  }

  gchar *groups = g_strdup_printf(ngettext("%" G_GSIZE_FORMAT " new group", "%" G_GSIZE_FORMAT " new groups",
                                          preview->groups), preview->groups);
  gchar *ungrouped = g_strdup_printf(ngettext("%" G_GSIZE_FORMAT " eligible image left ungrouped",
                                             "%" G_GSIZE_FORMAT " eligible images left ungrouped",
                                             preview->ungrouped), preview->ungrouped);
  gchar *skipped = g_strdup_printf(ngettext("%" G_GSIZE_FORMAT " image skipped",
                                           "%" G_GSIZE_FORMAT " images skipped",
                                           preview->skipped), preview->skipped);
  gchar *summary = g_strdup_printf("%s · %s\n%s", groups, ungrouped, skipped);
  gtk_label_set_text(GTK_LABEL(data->summary), summary);
  dt_free(summary);
  dt_free(skipped);
  dt_free(ungrouped);
  dt_free(groups);
}

static gboolean _auto_group_preview_start(gpointer user_data)
{
  GtkWidget *dialog = user_data;
  dt_auto_group_dialog_t *data = g_object_get_data(G_OBJECT(dialog), "auto-group-dialog");
  data->preview_source = 0;
  if(data->preview_running) return G_SOURCE_REMOVE;

  if(data->snapshot_dirty)
  {
    dt_auto_group_snapshot_t *snapshot = g_atomic_rc_box_new0(dt_auto_group_snapshot_t);
    const dt_auto_group_status_t status = dt_auto_group_snapshot_collect(snapshot);
    if(status != DT_AUTO_GROUP_STATUS_OK && status != DT_AUTO_GROUP_STATUS_EMPTY)
    {
      gtk_label_set_text(GTK_LABEL(data->summary), _("Preview unavailable."));
      g_atomic_rc_box_release_full(snapshot, _auto_group_snapshot_free);
      return G_SOURCE_REMOVE;
    }
    if(!IS_NULL_PTR(data->snapshot))
      g_atomic_rc_box_release_full(data->snapshot, _auto_group_snapshot_free);
    data->snapshot = snapshot;
    data->snapshot_dirty = FALSE;
  }

  dt_auto_group_preview_t *preview = g_new0(dt_auto_group_preview_t, 1);
  preview->snapshot = g_atomic_rc_box_acquire(data->snapshot);
  preview->interval = (int)dt_bauhaus_slider_get(data->interval);
  preview->generation = data->generation;
  GTask *task = g_task_new(dialog, NULL, _auto_group_preview_ready, NULL);
  g_task_set_task_data(task, preview, _auto_group_preview_free);
  data->preview_running = TRUE;
  g_task_run_in_thread(task, _auto_group_preview_worker);
  g_object_unref(task);
  return G_SOURCE_REMOVE;
}

static void _auto_group_update_duration(dt_auto_group_dialog_t *data)
{
  const int interval = (int)dt_bauhaus_slider_get(data->interval);
  char duration[9];
  g_snprintf(duration, sizeof(duration), "%02d:%02d:%02d", interval / 3600, (interval / 60) % 60, interval % 60);
  gtk_label_set_text(GTK_LABEL(data->duration), duration);
}

static void _auto_group_preview_changed(GtkWidget *widget G_GNUC_UNUSED, GtkWidget *dialog)
{
  dt_auto_group_dialog_t *data = g_object_get_data(G_OBJECT(dialog), "auto-group-dialog");
  _auto_group_update_duration(data);
  data->generation++;
  gtk_label_set_text(GTK_LABEL(data->summary), _("Calculating groups…"));
  if(data->preview_source) g_source_remove(data->preview_source);
  data->preview_source = g_timeout_add(150, _auto_group_preview_start, dialog);
}

static void _auto_group_collection_changed(gpointer instance G_GNUC_UNUSED, dt_collection_change_t change G_GNUC_UNUSED,
                                           dt_collection_properties_t property G_GNUC_UNUSED,
                                           gpointer imgs G_GNUC_UNUSED, int next G_GNUC_UNUSED,
                                           GtkWidget *dialog)
{
  dt_auto_group_dialog_t *data = g_object_get_data(G_OBJECT(dialog), "auto-group-dialog");
  data->snapshot_dirty = TRUE;
  _auto_group_preview_changed(NULL, dialog);
}

static void _auto_group_images_changed(gpointer instance G_GNUC_UNUSED, gpointer imgs G_GNUC_UNUSED, GtkWidget *dialog)
{
  dt_auto_group_dialog_t *data = g_object_get_data(G_OBJECT(dialog), "auto-group-dialog");
  data->snapshot_dirty = TRUE;
  _auto_group_preview_changed(NULL, dialog);
}

/** @brief Stop preview delivery and detach this dialog's numeric popup without restoring focus.
 * @details The parent can already be unmapping during destruction; the normal popup-hide API
 * restores focus to the slider and is therefore inappropriate on this teardown path.
 */
static void _auto_group_destroy(GtkWidget *dialog, dt_auto_group_dialog_t *data)
{
  data->closed = TRUE;
  dt_bauhaus_t *bauhaus = dt_bauhaus_get_global();
  if(bauhaus->current == DT_BAUHAUS_WIDGET(data->interval))
  {
    bauhaus->current = NULL;
    gtk_grab_remove(bauhaus->popup_area);
    gtk_widget_hide(bauhaus->popup_window);
    gtk_window_set_attached_to(GTK_WINDOW(bauhaus->popup_window), NULL);
  }
  if(data->preview_source) g_source_remove(data->preview_source);
  data->preview_source = 0;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT_ALL(dt_control_signal_get_global(), dialog);
  if(!IS_NULL_PTR(data->snapshot))
    g_atomic_rc_box_release_full(data->snapshot, _auto_group_snapshot_free);
  data->snapshot = NULL;
}

static void _auto_group_response(GtkDialog *dialog, gint response_id, dt_auto_group_dialog_t *data)
{
  if(response_id != GTK_RESPONSE_ACCEPT)
  {
    gtk_widget_destroy(GTK_WIDGET(dialog));
    dt_gui_refocus_center();
    return;
  }
  const int interval = (int)dt_bauhaus_slider_get(data->interval);
  dt_conf_set_int(AUTO_GROUP_INTERVAL_CONF, interval);

  dt_auto_group_snapshot_t snapshot = { 0 };
  dt_auto_group_plan_t plan = { 0 };
  dt_control_change_cursor_by_name_and_flush("progress");
  dt_auto_group_status_t status = dt_auto_group_snapshot_collect(&snapshot);
  if(status == DT_AUTO_GROUP_STATUS_OK) status = dt_auto_group_plan(&snapshot, interval, &plan);
  const size_t created_groups = plan.groups;
  if(status == DT_AUTO_GROUP_STATUS_OK) status = dt_auto_group_execute(&plan);
  dt_auto_group_plan_cleanup(&plan);
  dt_auto_group_snapshot_cleanup(&snapshot);
  dt_control_commit_cursor();

  if(status == DT_AUTO_GROUP_STATUS_OK)
    dt_control_log(ngettext("%" G_GSIZE_FORMAT " group created", "%" G_GSIZE_FORMAT " groups created", created_groups),
                   created_groups);
  else if(status == DT_AUTO_GROUP_STATUS_EMPTY)
    dt_control_log(_("No images qualify for automatic grouping."));
  else
    dt_control_log(_("Automatic grouping failed."));
  gtk_widget_destroy(GTK_WIDGET(dialog));
  dt_gui_refocus_center();
}

static GtkWidget *_auto_group_dialog_new(GtkWindow *parent)
{
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Auto group images"), parent,
                                                  GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("Cancel"), GTK_RESPONSE_CANCEL, _("Group"), GTK_RESPONSE_ACCEPT, NULL);
  dt_auto_group_dialog_t *data = g_new0(dt_auto_group_dialog_t, 1);
  gtk_widget_set_name(dialog, "auto-group-dialog");
  data->snapshot_dirty = TRUE;
  g_object_set_data_full(G_OBJECT(dialog), "auto-group-dialog", data, g_free);
  const int spacing = DT_GUI_BOX_SPACING;
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 3 * spacing);
  gtk_box_set_spacing(GTK_BOX(content), 3 * spacing);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 36 * DT_GUI_EM_SIZE, -1);

  data->duration = gtk_label_new(NULL);
  gtk_widget_set_halign(data->duration, GTK_ALIGN_CENTER);
  data->interval = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), 0, 3600, 1,
                                                   CLAMP(dt_conf_get_int(AUTO_GROUP_INTERVAL_CONF), 0, 3600), 0);
  dt_bauhaus_widget_set_label(data->interval, _("maximum time between images"));
  gtk_widget_set_tooltip_text(data->interval, _("Maximum gap in seconds. Right-click to enter a value."));
  atk_object_set_name(gtk_widget_get_accessible(data->interval), _("Maximum time between images"));
  gtk_box_pack_start(GTK_BOX(content), data->duration, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), data->interval, FALSE, FALSE, 0);

  data->summary = gtk_label_new(_("Calculating groups…"));
  gtk_label_set_xalign(GTK_LABEL(data->summary), 0.0);
  gtk_label_set_line_wrap(GTK_LABEL(data->summary), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(data->summary), 50);
  gtk_widget_set_tooltip_text(data->summary, _("Existing groups and images without a capture time are skipped."));
  gtk_box_pack_start(GTK_BOX(content), data->summary, FALSE, FALSE, spacing);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  g_signal_connect(data->interval, "value-changed", G_CALLBACK(_auto_group_preview_changed), dialog);
  g_signal_connect(dialog, "response", G_CALLBACK(_auto_group_response), data);
  g_signal_connect(dialog, "destroy", G_CALLBACK(_auto_group_destroy), data);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_COLLECTION_CHANGED,
                                  G_CALLBACK(_auto_group_collection_changed), dialog);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_IMAGE_INFO_CHANGED,
                                  G_CALLBACK(_auto_group_images_changed), dialog);
  _auto_group_preview_changed(NULL, dialog);
  return dialog;
}

void dt_gui_auto_group_show(GtkWindow *parent)
{
  GtkWidget *dialog = _auto_group_dialog_new(parent);
  gtk_widget_show_all(dialog);
  dt_auto_group_dialog_t *data = g_object_get_data(G_OBJECT(dialog), "auto-group-dialog");
  gtk_widget_grab_focus(data->interval);
}
