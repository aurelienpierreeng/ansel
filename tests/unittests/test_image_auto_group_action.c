/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>

#include <cmocka.h>

#include "common/conf.h"
#include "darktable.h"
#include "../../src/gui/auto_group.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_conf_t _conf = { 0 };
static dt_control_t _control;
static gboolean _cursor_busy;
static int _refocus_calls;
static dt_bauhaus_t _bauhaus = { 0 };
static GtkWidget *_popup_root;

static GtkWidget *_popup_root_window(void)
{
  return _popup_root;
}
static int _snapshot_calls = 0;
static int _persisted_interval = -1;
static dt_auto_group_status_t _snapshot_status = DT_AUTO_GROUP_STATUS_EMPTY;
static int _execute_calls;
static size_t _executed_groups;
static dt_auto_group_image_t _images[] = {
  { .imgid = 1, .group_id = 1, .group_members = 1, .datetime_taken = 1000000 },
  { .imgid = 2, .group_id = 2, .group_members = 1, .datetime_taken = 2000000 },
  { .imgid = 3, .group_id = 3, .group_members = 1, .datetime_taken = 10000000 },
  { .imgid = 4, .group_id = 4, .group_members = 2, .datetime_taken = 1000000 },
  { .imgid = 5, .group_id = 4, .group_members = 2, .datetime_taken = 1000000 },
};

void dt_control_change_cursor_by_name_and_flush(const char *curs_str)
{
  assert_string_equal(curs_str, "progress");
  assert_false(_cursor_busy);
  _cursor_busy = TRUE;
}

void dt_control_commit_cursor(void)
{
  assert_true(_cursor_busy);
  _cursor_busy = FALSE;
}

void dt_gui_refocus_center(void)
{
  _refocus_calls++;
}

dt_auto_group_status_t dt_auto_group_snapshot_collect(dt_auto_group_snapshot_t *snapshot)
{
  _snapshot_calls++;
  _persisted_interval = dt_conf_get_int(AUTO_GROUP_INTERVAL_CONF);
  if(_snapshot_status == DT_AUTO_GROUP_STATUS_OK)
  {
    snapshot->images = g_malloc(sizeof(_images));
    memcpy(snapshot->images, _images, sizeof(_images));
    snapshot->count = G_N_ELEMENTS(_images);
  }
  return _snapshot_status;
}

dt_auto_group_status_t dt_auto_group_execute(const dt_auto_group_plan_t *plan)
{
  _execute_calls++;
  _executed_groups = plan->groups;
  return DT_AUTO_GROUP_STATUS_OK;
}

static int _setup(void **state G_GNUC_UNUSED)
{
  _control = (dt_control_t){ 0 };
  darktable.control = &_control;
  dt_pthread_mutex_init(&_control.log_mutex, NULL);
  dt_pthread_mutex_init(&_control.run_mutex, NULL);
  _cursor_busy = FALSE;
  _refocus_calls = 0;
  darktable.conf = &_conf;
  darktable.bauhaus = &_bauhaus;
  if(IS_NULL_PTR(darktable.signals)) darktable.signals = dt_control_signal_init();
  dt_conf_init(&_conf, "/dev/null", NULL);
  dt_conf_set_int(AUTO_GROUP_INTERVAL_CONF, 1);
  _snapshot_calls = 0;
  _persisted_interval = -1;
  _snapshot_status = DT_AUTO_GROUP_STATUS_EMPTY;
  _execute_calls = 0;
  _executed_groups = 0;
  return 0;
}

static int _teardown(void **state G_GNUC_UNUSED)
{
  while(g_main_context_iteration(NULL, FALSE));
  if(_control.log_message_timeout_id) g_source_remove(_control.log_message_timeout_id);
  dt_pthread_mutex_destroy(&_control.log_mutex);
  dt_pthread_mutex_destroy(&_control.run_mutex);
  darktable.control = NULL;
  assert_false(_cursor_busy);
  dt_conf_cleanup(&_conf);
  darktable.conf = NULL;
  darktable.bauhaus = NULL;
  return 0;
}

static dt_auto_group_dialog_t *_dialog_data(GtkWidget *dialog)
{
  return g_object_get_data(G_OBJECT(dialog), "auto-group-dialog");
}

static void test_auto_group_dialog_defaults_and_updates_duration(void **state G_GNUC_UNUSED)
{
  GtkWidget *const dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *const data = _dialog_data(dialog);
  assert_true(gtk_widget_has_default(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)));
  assert_int_equal(dt_bauhaus_slider_get(data->interval), 1);
  assert_string_equal(gtk_label_get_text(GTK_LABEL(data->duration)), "00:00:01");

  dt_bauhaus_slider_set(data->interval, 0);
  assert_string_equal(gtk_label_get_text(GTK_LABEL(data->duration)), "00:00:00");
  dt_bauhaus_slider_set(data->interval, 3600);
  assert_string_equal(gtk_label_get_text(GTK_LABEL(data->duration)), "01:00:00");
  char *seconds = dt_bauhaus_slider_get_text(data->interval, dt_bauhaus_slider_get(data->interval));
  assert_string_equal(seconds, "3600");
  dt_free(seconds);
  gtk_widget_destroy(dialog);
}

static void test_auto_group_dialog_clamps_native_numeric_input(void **state G_GNUC_UNUSED)
{
  GtkWidget *const dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *const data = _dialog_data(dialog);
  dt_bauhaus_slider_set(data->interval, -1);
  assert_int_equal(dt_bauhaus_slider_get(data->interval), 0);
  dt_bauhaus_slider_set(data->interval, 3601);
  assert_int_equal(dt_bauhaus_slider_get(data->interval), 3600);
  gtk_widget_destroy(dialog);
}

static void test_auto_group_cancel_does_not_persist_or_execute(void **state G_GNUC_UNUSED)
{
  dt_conf_set_int(AUTO_GROUP_INTERVAL_CONF, 12);
  GtkWidget *const dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *const data = _dialog_data(dialog);
  dt_bauhaus_slider_set(data->interval, 3600);
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  assert_int_equal(dt_conf_get_int(AUTO_GROUP_INTERVAL_CONF), 12);
  assert_int_equal(_snapshot_calls, 0);
  assert_int_equal(_refocus_calls, 1);
}

static void test_auto_group_persists_before_empty_execution(void **state G_GNUC_UNUSED)
{
  GtkWidget *const dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *const data = _dialog_data(dialog);
  dt_bauhaus_slider_set(data->interval, 0);
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  assert_true(_snapshot_calls > 0);
  assert_int_equal(_persisted_interval, 0);
  assert_int_equal(dt_conf_get_int(AUTO_GROUP_INTERVAL_CONF), 0);
}

static void test_auto_group_persists_before_failed_snapshot(void **state G_GNUC_UNUSED)
{
  _snapshot_status = DT_AUTO_GROUP_STATUS_ERROR;
  GtkWidget *const dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *const data = _dialog_data(dialog);
  dt_bauhaus_slider_set(data->interval, 3600);
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  assert_true(_snapshot_calls > 0);
  assert_int_equal(_persisted_interval, 3600);
  assert_int_equal(dt_conf_get_int(AUTO_GROUP_INTERVAL_CONF), 3600);
}

static void _wait_for_preview(GtkWidget *dialog)
{
  const dt_auto_group_dialog_t *data = _dialog_data(dialog);
  const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while((data->preview_source || data->preview_running) && g_get_monotonic_time() < deadline)
  {
    while(g_main_context_iteration(NULL, FALSE));
    g_usleep(1000);
  }
  assert_false(data->preview_running);
  assert_int_equal(data->preview_source, 0);
}

static void test_preview_counts_and_reuses_snapshot(void **state G_GNUC_UNUSED)
{
  _snapshot_status = DT_AUTO_GROUP_STATUS_OK;
  GtkWidget *dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *data = _dialog_data(dialog);
  _wait_for_preview(dialog);
  assert_string_equal(gtk_label_get_text(GTK_LABEL(data->summary)),
                       "1 new group · 1 eligible image left ungrouped\n"
                       "2 images skipped");
  dt_bauhaus_slider_set(data->interval, 10);
  _wait_for_preview(dialog);
  assert_string_equal(gtk_label_get_text(GTK_LABEL(data->summary)),
                       "1 new group · 0 eligible images left ungrouped\n"
                       "2 images skipped");
  assert_int_equal(_snapshot_calls, 1);
  assert_int_equal(_execute_calls, 0);
  assert_int_equal(dt_conf_get_int(AUTO_GROUP_INTERVAL_CONF), 1);
  gtk_widget_destroy(dialog);
}

static void test_preview_discards_superseded_result_and_refreshes_snapshot(void **state G_GNUC_UNUSED)
{
  _snapshot_status = DT_AUTO_GROUP_STATUS_OK;
  GtkWidget *dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *data = _dialog_data(dialog);
  g_source_remove(data->preview_source);
  _auto_group_preview_start(dialog);
  assert_true(data->preview_running);
  dt_bauhaus_slider_set(data->interval, 0);
  _auto_group_images_changed(NULL, NULL, dialog);
  _wait_for_preview(dialog);
  assert_string_equal(gtk_label_get_text(GTK_LABEL(data->summary)),
                       "0 new groups · 3 eligible images left ungrouped\n"
                       "2 images skipped");
  assert_int_equal(_snapshot_calls, 2);
  gtk_widget_destroy(dialog);
}

static void test_destroy_during_preview_releases_dialog(void **state G_GNUC_UNUSED)
{
  _snapshot_status = DT_AUTO_GROUP_STATUS_OK;
  GtkWidget *dialog = _auto_group_dialog_new(NULL);
  const dt_auto_group_dialog_t *data = _dialog_data(dialog);
  gpointer alive = dialog;
  g_object_add_weak_pointer(G_OBJECT(dialog), &alive);
  g_source_remove(data->preview_source);
  _auto_group_preview_start(dialog);
  gtk_widget_destroy(dialog);
  const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while(!IS_NULL_PTR(alive) && g_get_monotonic_time() < deadline)
  {
    while(g_main_context_iteration(NULL, FALSE));
    g_usleep(1000);
  }
  assert_null(alive);
  assert_int_equal(_execute_calls, 0);
}

static void test_apply_revalidates_with_current_seconds(void **state G_GNUC_UNUSED)
{
  _snapshot_status = DT_AUTO_GROUP_STATUS_OK;
  GtkWidget *dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *data = _dialog_data(dialog);
  _wait_for_preview(dialog);
  dt_bauhaus_slider_set(data->interval, 323);
  assert_string_equal(gtk_label_get_text(GTK_LABEL(data->duration)), "00:05:23");
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  assert_int_equal(_snapshot_calls, 2);
  assert_int_equal(_execute_calls, 1);
  assert_int_equal(_executed_groups, 1);
  assert_int_equal(dt_conf_get_int(AUTO_GROUP_INTERVAL_CONF), 323);
}

static void test_standard_right_click_popup_and_dialog_teardown(void **state G_GNUC_UNUSED)
{
  _popup_root = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  dt_widget_set_root_window_handler(_popup_root_window);
  dt_accels_t *accels = dt_accels_init("/dev/null", GTK_ACCEL_VISIBLE);
  dt_accels_set_global(accels);
  dt_bauhaus_t *bauhaus = dt_bauhaus_init();
  darktable.bauhaus = bauhaus;
  GtkWidget *dialog = _auto_group_dialog_new(NULL);
  dt_auto_group_dialog_t *data = _dialog_data(dialog);
  gtk_widget_show_all(dialog);
  _wait_for_preview(dialog);
  GdkEventButton event = { .type = GDK_BUTTON_PRESS, .button = 3,
                           .window = gtk_widget_get_window(data->interval),
                           .x = gtk_widget_get_allocated_width(data->interval) / 2.0,
                           .y = gtk_widget_get_allocated_height(data->interval) / 2.0 };
  gboolean handled = FALSE;
  g_signal_emit_by_name(data->interval, "button-press-event", &event, &handled);
  assert_true(handled);
  assert_ptr_equal(bauhaus->current, DT_BAUHAUS_WIDGET(data->interval));
  assert_true(gtk_widget_get_visible(bauhaus->popup_window));
  assert_ptr_equal(gtk_window_get_transient_for(GTK_WINDOW(bauhaus->popup_window)), GTK_WINDOW(dialog));
  const char digits[] = "323";
  for(size_t index = 0; index < strlen(digits); index++)
  {
    char text[] = { digits[index], '\0' };
    GdkEventKey key = { .type = GDK_KEY_PRESS, .keyval = digits[index], .string = text, .length = 1 };
    g_signal_emit_by_name(bauhaus->popup_area, "key-press-event", &key, &handled);
    assert_true(handled);
  }
  GdkEventKey key = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_Return, .string = "", .length = 0 };
  g_signal_emit_by_name(bauhaus->popup_area, "key-press-event", &key, &handled);
  assert_true(handled);
  assert_int_equal(dt_bauhaus_slider_get(data->interval), 323);
  assert_string_equal(gtk_label_get_text(GTK_LABEL(data->duration)), "00:05:23");
  assert_false(gtk_widget_get_visible(bauhaus->popup_window));
  assert_true(gtk_widget_get_visible(dialog));
  assert_int_equal(_execute_calls, 0);
  g_signal_emit_by_name(data->interval, "button-press-event", &event, &handled);
  key.keyval = GDK_KEY_Escape;
  g_signal_emit_by_name(bauhaus->popup_area, "key-press-event", &key, &handled);
  assert_false(gtk_widget_get_visible(bauhaus->popup_window));
  assert_true(gtk_widget_get_visible(dialog));
  g_signal_emit_by_name(data->interval, "button-press-event", &event, &handled);
  assert_ptr_equal(bauhaus->current, DT_BAUHAUS_WIDGET(data->interval));
  gtk_widget_destroy(dialog);
  assert_null(bauhaus->current);
  assert_false(gtk_widget_get_visible(bauhaus->popup_window));
  gtk_widget_destroy(bauhaus->popup_window);
  pango_font_description_free(bauhaus->pango_font_desc);
  dt_free(bauhaus);
  darktable.bauhaus = &_bauhaus;
  dt_accels_cleanup(accels);
  dt_accels_set_global(NULL);
  dt_widget_set_root_window_handler(NULL);
  gtk_widget_destroy(_popup_root);
  _popup_root = NULL;
}

int main(int argc, char *argv[])
{
  assert_true(gtk_init_check(&argc, &argv));
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_auto_group_dialog_defaults_and_updates_duration, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_auto_group_dialog_clamps_native_numeric_input, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_auto_group_cancel_does_not_persist_or_execute, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_auto_group_persists_before_empty_execution, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_auto_group_persists_before_failed_snapshot, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_preview_counts_and_reuses_snapshot, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_preview_discards_superseded_result_and_refreshes_snapshot, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_destroy_during_preview_releases_dialog, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_apply_revalidates_with_current_seconds, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_standard_right_click_popup_and_dialog_teardown, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
