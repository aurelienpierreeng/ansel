/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "common/auto_group.h"
#include "common/conf.h"
#include "common/datetime.h"
#include "common/file_location.h"
#include "common/grouping.h"
#include "common/image_notify.h"
#include "common/undo.h"
#include "caches/image_cache.h"
#include "control/control.h"
#include "control/signal.h"
#include "darktable.h"
#include "imageio/imageio_module.h"
#include "testdb.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

typedef struct dt_auto_group_notifications_t
{
  int image_info_changed;
  GList *imgids;
} dt_auto_group_notifications_t;

static char *_rawspeed_datadir = NULL;
static dt_imageio_t _imageio = { 0 };
static dt_conf_t _conf = { 0 };
static unsigned _restore_failures;

static void _dispatch_image_changes(GList *imgids)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_IMAGE_INFO_CHANGED, imgids);
}

static void _record_restore_failure(const char *message)
{
  assert_string_equal(message, "automatic grouping could not be restored");
  _restore_failures++;
}

static int _setup(void **state)
{
  if(testdb_setup(state)) return -1;

  dt_image_cache_init(FALSE);
  darktable.conf = &_conf;
  dt_conf_init(&_conf, "/dev/null", NULL);

  _rawspeed_datadir = g_dir_make_tmp("ansel-test-rawspeed-XXXXXX", NULL);
  if(IS_NULL_PTR(_rawspeed_datadir)) return -1;

  char *const rawspeed_dir = g_build_filename(_rawspeed_datadir, "rawspeed", NULL);
  char *const cameras_xml = g_build_filename(rawspeed_dir, "cameras.xml", NULL);
  char *const source_xml = g_build_filename(ANSEL_TEST_SOURCE_DIR, "src", "external", "rawspeed", "data",
                                            "cameras.xml", NULL);
  GFile *const source_file = g_file_new_for_path(source_xml);
  GFile *const destination_file = g_file_new_for_path(cameras_xml);
  const gboolean copied = g_mkdir(rawspeed_dir, 0700) == 0
                          && g_file_copy(source_file, destination_file, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
  g_object_unref(destination_file);
  g_object_unref(source_file);
  dt_free(source_xml);
  dt_free(cameras_xml);
  dt_free(rawspeed_dir);
  if(!copied) return -1;

  dt_loc_init_datadir(NULL, _rawspeed_datadir);
  darktable.imageio = &_imageio;
  return 0;
}

static int _teardown(void **state)
{
  char *const rawspeed_dir = g_build_filename(_rawspeed_datadir, "rawspeed", NULL);
  char *const cameras_xml = g_build_filename(rawspeed_dir, "cameras.xml", NULL);
  g_remove(cameras_xml);
  g_rmdir(rawspeed_dir);
  g_rmdir(_rawspeed_datadir);
  dt_free(cameras_xml);
  dt_free(rawspeed_dir);
  dt_conf_cleanup(&_conf);
  darktable.conf = NULL;
  dt_free(darktable.datadir);
  darktable.datadir = NULL;
  darktable.imageio = NULL;
  dt_image_cache_cleanup();
  dt_free(_rawspeed_datadir);
  _rawspeed_datadir = NULL;
  return testdb_teardown(state);
}

static void _image_info_changed(gpointer instance G_GNUC_UNUSED, GList *imgids, gpointer user_data)
{
  dt_auto_group_notifications_t *const notifications = user_data;
  notifications->image_info_changed++;
  g_list_free(notifications->imgids);
  notifications->imgids = g_list_copy(imgids);
}

static void _dispatch_pending_events(void)
{
  while(g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

static void _start_gui_services(dt_control_t *control)
{
  *control = (dt_control_t){ 0 };
  dt_pthread_mutex_init(&control->run_mutex, NULL);
  control->gui_thread = pthread_self();
  control->running = TRUE;
  darktable.control = control;
  dt_image_notify_set_changed_handler(_dispatch_image_changes);
  dt_auto_group_set_message_handler(_record_restore_failure);
  _restore_failures = 0;
  if(IS_NULL_PTR(darktable.signals))
  {
    darktable.signals = dt_control_signal_init();
    dt_image_cache_connect_info_changed_first(darktable.signals);
  }
}

static void _stop_gui_services(dt_control_t *control)
{
  _dispatch_pending_events();
  darktable.control = NULL;
  dt_pthread_mutex_destroy(&control->run_mutex);
}

static void assert_assignment(const dt_auto_group_plan_t *plan, const size_t index, const int32_t imgid,
                              const int32_t group_id)
{
  assert_int_equal(plan->assignments[index].imgid, imgid);
  assert_int_equal(plan->assignments[index].expected_group_id, imgid);
  assert_int_equal(plan->assignments[index].new_group_id, group_id);
}

static void test_plan_groups_unordered_consecutive_matching_images(void **state G_GNUC_UNUSED)
{
  dt_auto_group_image_t images[] = {
    { .imgid = 9, .group_id = 9, .group_members = 1, .film_id = 2, .datetime_taken = 2000000, .camera_maker = "B", .camera_model = "2" },
    { .imgid = 4, .group_id = 4, .group_members = 1, .film_id = 1, .datetime_taken = 3000001, .camera_maker = "A", .camera_model = "1" },
    { .imgid = 8, .group_id = 8, .group_members = 1, .film_id = 2, .datetime_taken = 2000000, .camera_maker = "B", .camera_model = "2" },
    { .imgid = 3, .group_id = 3, .group_members = 1, .film_id = 1, .datetime_taken = 2000000, .camera_maker = "A", .camera_model = "1" },
    { .imgid = 2, .group_id = 2, .group_members = 1, .film_id = 1, .datetime_taken = 1000000, .camera_maker = "A", .camera_model = "1" },
  };
  const dt_auto_group_snapshot_t snapshot = { .images = images, .count = G_N_ELEMENTS(images) };
  dt_auto_group_plan_t plan = { 0 };

  assert_int_equal(dt_auto_group_plan(&snapshot, 1, &plan), DT_AUTO_GROUP_STATUS_OK);
  assert_int_equal(plan.count, 4);
  assert_int_equal(plan.groups, 2);
  assert_int_equal(plan.ungrouped, 1);
  assert_int_equal(plan.skipped, 0);
  assert_assignment(&plan, 0, 2, 2);
  assert_assignment(&plan, 1, 3, 2);
  assert_assignment(&plan, 2, 8, 8);
  assert_assignment(&plan, 3, 9, 8);
  dt_auto_group_plan_cleanup(&plan);
}

static void test_plan_skips_existing_groups_missing_timestamps_and_singletons(void **state G_GNUC_UNUSED)
{
  dt_auto_group_image_t images[] = {
    { .imgid = 1, .group_id = 1, .group_members = 2, .film_id = 1, .datetime_taken = 1000000, .camera_maker = "A", .camera_model = "1" },
    { .imgid = 2, .group_id = 1, .film_id = 1, .datetime_taken = 1000000, .camera_maker = "A", .camera_model = "1" },
    { .imgid = 3, .group_id = 3, .group_members = 1, .film_id = 1, .camera_maker = "A", .camera_model = "1" },
    { .imgid = 4, .group_id = 4, .group_members = 1, .film_id = 1, .datetime_taken = 1000000, .camera_maker = "B", .camera_model = "1" },
  };
  const dt_auto_group_snapshot_t snapshot = { .images = images, .count = G_N_ELEMENTS(images) };
  dt_auto_group_plan_t plan = { 0 };

  assert_int_equal(dt_auto_group_plan(&snapshot, 3600, &plan), DT_AUTO_GROUP_STATUS_EMPTY);
  assert_null(plan.assignments);
  assert_int_equal(plan.count, 0);
}

static void test_plan_zero_interval_groups_only_exact_microsecond_timestamps(void **state G_GNUC_UNUSED)
{
  dt_auto_group_image_t images[] = {
    { .imgid = 5, .group_id = 5, .group_members = 1, .film_id = 1, .datetime_taken = 1234568, .camera_maker = "A", .camera_model = "1" },
    { .imgid = 3, .group_id = 3, .group_members = 1, .film_id = 1, .datetime_taken = 1234567, .camera_maker = "A", .camera_model = "1" },
    { .imgid = 2, .group_id = 2, .group_members = 1, .film_id = 1, .datetime_taken = 1234567, .camera_maker = "A", .camera_model = "1" },
  };
  const dt_auto_group_snapshot_t snapshot = { .images = images, .count = G_N_ELEMENTS(images) };
  dt_auto_group_plan_t plan = { 0 };

  assert_int_equal(dt_auto_group_plan(&snapshot, 0, &plan), DT_AUTO_GROUP_STATUS_OK);
  assert_int_equal(plan.count, 2);
  assert_assignment(&plan, 0, 2, 2);
  assert_assignment(&plan, 1, 3, 2);
  dt_auto_group_plan_cleanup(&plan);
}

static void test_plan_rejects_an_out_of_range_interval(void **state G_GNUC_UNUSED)
{
  const dt_auto_group_snapshot_t snapshot = { 0 };
  dt_auto_group_plan_t plan = { 0 };

  assert_int_equal(dt_auto_group_plan(&snapshot, -1, &plan), DT_AUTO_GROUP_STATUS_ERROR);
  assert_int_equal(dt_auto_group_plan(&snapshot, 3601, &plan), DT_AUTO_GROUP_STATUS_ERROR);
}

static int _deny_collected_images(void *user_data G_GNUC_UNUSED, const int operation, const char *arg1,
                                   const char *arg2 G_GNUC_UNUSED, const char *database_name G_GNUC_UNUSED,
                                   const char *trigger_name G_GNUC_UNUSED)
{
  return operation == SQLITE_READ && !g_strcmp0(arg1, "images") ? SQLITE_DENY : SQLITE_OK;
}

static int _deny_transaction(void *user_data, const int operation, const char *operation_name,
                             const char *arg2 G_GNUC_UNUSED, const char *database_name G_GNUC_UNUSED,
                             const char *trigger_name G_GNUC_UNUSED)
{
  return operation == SQLITE_TRANSACTION && !g_strcmp0(operation_name, user_data) ? SQLITE_DENY : SQLITE_OK;
}

static void test_snapshot_discards_rows_when_collection_read_fails(void **state G_GNUC_UNUSED)
{
  dt_auto_group_snapshot_t snapshot = {
    .images = g_new0(dt_auto_group_image_t, 1),
    .count = 1,
  };
  sqlite3 *const database = dt_database_get_sqlite3_global();

  assert_int_equal(sqlite3_set_authorizer(database, _deny_collected_images, NULL), SQLITE_OK);
  assert_int_equal(dt_auto_group_snapshot_collect(&snapshot), DT_AUTO_GROUP_STATUS_ERROR);
  assert_int_equal(sqlite3_set_authorizer(database, NULL, NULL), SQLITE_OK);
  assert_null(snapshot.images);
  assert_int_equal(snapshot.count, 0);
}

static void test_snapshot_normalizes_camera_identity_through_rawspeed(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/auto-group-snapshot");
  const int32_t first = testdb_make_image(film_id, "first.raw");
  const int32_t second = testdb_make_image(film_id, "second.raw");
  const int64_t datetime_taken = G_GINT64_CONSTANT(63800000000000000);
  dt_image_t image;

  dt_image_init(&image);
  image.id = first;
  image.film_id = film_id;
  image.group_id = first;
  image.exif_datetime_taken = datetime_taken;
  g_strlcpy(image.filename, "first.raw", sizeof(image.filename));
  g_strlcpy(image.exif_maker, "Canon", sizeof(image.exif_maker));
  g_strlcpy(image.exif_model, "EOS 5D", sizeof(image.exif_model));
  dt_image_repository_store(&image);
  image.id = second;
  image.group_id = second;
  image.exif_datetime_taken = datetime_taken + 1;
  g_strlcpy(image.filename, "second.raw", sizeof(image.filename));
  dt_image_repository_store(&image);

  char *const collected = g_strdup_printf("INSERT INTO memory.collected_images (imgid) VALUES (%d), (%d)", first,
                                          second);
  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(), collected, NULL, NULL, NULL), SQLITE_OK);
  dt_free(collected);

  dt_auto_group_snapshot_t snapshot = { 0 };
  assert_int_equal(dt_auto_group_snapshot_collect(&snapshot), DT_AUTO_GROUP_STATUS_OK);
  assert_int_equal(snapshot.count, 2);
  assert_string_equal(snapshot.images[0].camera_maker, "Canon");
  assert_string_equal(snapshot.images[0].camera_model, "EOS 5D");
  dt_auto_group_snapshot_cleanup(&snapshot);
}

static void test_new_image_starts_in_its_own_group(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/auto-group");
  const int32_t imgid = testdb_make_image(film_id, "image.raw");
  dt_image_t image;
  dt_auto_group_plan_t plan = { 0 };

  dt_image_init(&image);
  image.id = imgid;
  image.film_id = film_id;
  image.group_id = imgid;
  g_strlcpy(image.filename, "image.raw", sizeof(image.filename));
  dt_image_repository_store(&image);
  GList *members = dt_image_repository_get_group_members(imgid, -1);
  assert_int_equal(g_list_length(members), 1);
  assert_int_equal(GPOINTER_TO_INT(members->data), imgid);
  g_list_free(members);
  assert_null(plan.assignments);
  assert_int_equal(plan.count, 0);
}

static void test_execute_commits_batch_and_notifies_once(void **state G_GNUC_UNUSED)
{
  dt_control_t control;
  dt_auto_group_notifications_t notifications = { 0 };
  const int32_t film_id = testdb_make_film("/testdb/auto-group-execute");
  const int32_t images[] = {
    testdb_make_image(film_id, "a.raw"), testdb_make_image(film_id, "b.raw"),
    testdb_make_image(film_id, "c.raw"), testdb_make_image(film_id, "d.raw"),
  };
  dt_image_group_assignment_t assignments[] = {
    { .imgid = images[0], .expected_group_id = images[0], .new_group_id = images[0] },
    { .imgid = images[1], .expected_group_id = images[1], .new_group_id = images[0] },
    { .imgid = images[2], .expected_group_id = images[2], .new_group_id = images[2] },
    { .imgid = images[3], .expected_group_id = images[3], .new_group_id = images[2] },
  };
  const dt_auto_group_plan_t plan = { .assignments = assignments,
                                      .count = G_N_ELEMENTS(assignments) };

  for(size_t index = 0; index < G_N_ELEMENTS(images); index++)
    assert_true(dt_image_repository_set_group(images[index], images[index]));

  _start_gui_services(&control);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, G_CALLBACK(_image_info_changed),
                            &notifications);

  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_OK);
  _dispatch_pending_events();
  assert_int_equal(notifications.image_info_changed, 1);
  assert_int_equal(g_list_length(notifications.imgids), G_N_ELEMENTS(assignments));
  for(size_t index = 0; index < G_N_ELEMENTS(images); index++)
    assert_int_equal(GPOINTER_TO_INT(g_list_nth_data(notifications.imgids, index)), images[index]);
  g_list_free(notifications.imgids);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_image_info_changed), &notifications);
  _stop_gui_services(&control);

  for(size_t index = 0; index < G_N_ELEMENTS(images); index++)
  {
    GList *const members = dt_image_repository_get_group_members(assignments[index].new_group_id, -1);
    assert_int_equal(g_list_length(members), 2);
    g_list_free(members);
  }
}

static void test_execute_rolls_back_late_conflict_without_notifications(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/auto-group-conflict");
  const int32_t a = testdb_make_image(film_id, "a.raw");
  const int32_t b = testdb_make_image(film_id, "b.raw");
  dt_image_group_assignment_t assignments[] = {
    { .imgid = a, .expected_group_id = a, .new_group_id = b },
    { .imgid = b, .expected_group_id = a, .new_group_id = b },
  };
  const dt_auto_group_plan_t plan = { .assignments = assignments,
                                      .count = G_N_ELEMENTS(assignments) };

  assert_true(dt_image_repository_set_group(a, a));
  assert_true(dt_image_repository_set_group(b, b));
  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_CONFLICT);
  GList *members = dt_image_repository_get_group_members(a, -1);
  assert_int_equal(g_list_length(members), 1);
  g_list_free(members);
  members = dt_image_repository_get_group_members(b, -1);
  assert_int_equal(g_list_length(members), 1);
  g_list_free(members);
}

static void test_execute_empty_plan_does_not_write_or_notify(void **state G_GNUC_UNUSED)
{
  const dt_auto_group_plan_t plan = { 0 };

  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_EMPTY);
}

static void test_execute_suppresses_notifications_when_begin_fails(void **state G_GNUC_UNUSED)
{
  dt_control_t control;
  dt_auto_group_notifications_t notifications = { 0 };
  const int32_t film_id = testdb_make_film("/testdb/auto-group-begin-failure");
  const int32_t imgid = testdb_make_image(film_id, "image.raw");
  dt_image_group_assignment_t assignment = { .imgid = imgid, .expected_group_id = imgid,
                                                    .new_group_id = imgid };
  const dt_auto_group_plan_t plan = { .assignments = &assignment, .count = 1 };
  sqlite3 *const database = dt_database_get_sqlite3_global();

  assert_true(dt_image_repository_set_group(imgid, imgid));
  _start_gui_services(&control);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, G_CALLBACK(_image_info_changed),
                            &notifications);
  assert_int_equal(sqlite3_set_authorizer(database, _deny_transaction, "BEGIN"), SQLITE_OK);
  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_ERROR);
  assert_int_equal(sqlite3_set_authorizer(database, NULL, NULL), SQLITE_OK);
  _dispatch_pending_events();
  assert_int_equal(notifications.image_info_changed, 0);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_image_info_changed), &notifications);
  _stop_gui_services(&control);
}

/** @brief Create two singleton rows and a caller-owned plan that joins them under the first image. */
static void _create_pair_fixture(const char *folder, dt_image_group_assignment_t assignments[2])
{
  const int32_t film_id = testdb_make_film(folder);
  const int32_t leader = testdb_make_image(film_id, "leader.raw");
  const int32_t member = testdb_make_image(film_id, "member.raw");
  assert_true(dt_image_repository_set_group(leader, leader));
  assert_true(dt_image_repository_set_group(member, member));
  assignments[0] = (dt_image_group_assignment_t){ .imgid = leader, .expected_group_id = leader,
                                                 .new_group_id = leader };
  assignments[1] = (dt_image_group_assignment_t){ .imgid = member, .expected_group_id = member,
                                                 .new_group_id = leader };
}

static void test_execute_rolls_back_and_suppresses_notifications_when_commit_fails(void **state G_GNUC_UNUSED)
{
  dt_control_t control;
  dt_auto_group_notifications_t notifications = { 0 };
  dt_image_group_assignment_t assignments[2];
  _create_pair_fixture("/testdb/auto-group-commit-failure", assignments);
  const int32_t leader = assignments[0].imgid;
  const dt_auto_group_plan_t plan = { .assignments = assignments,
                                      .count = G_N_ELEMENTS(assignments) };
  sqlite3 *const database = dt_database_get_sqlite3_global();

  _start_gui_services(&control);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, G_CALLBACK(_image_info_changed),
                            &notifications);
  assert_int_equal(sqlite3_set_authorizer(database, _deny_transaction, "COMMIT"), SQLITE_OK);
  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_ERROR);
  assert_int_equal(sqlite3_set_authorizer(database, NULL, NULL), SQLITE_OK);
  _dispatch_pending_events();
  assert_int_equal(notifications.image_info_changed, 0);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_image_info_changed), &notifications);
  _stop_gui_services(&control);

  GList *const members = dt_image_repository_get_group_members(leader, -1);
  assert_int_equal(g_list_length(members), 1);
  g_list_free(members);
}

static void _count_undo_calls(gpointer user_data G_GNUC_UNUSED, dt_undo_type_t type G_GNUC_UNUSED, dt_undo_data_t item,
                              dt_undo_action_t action G_GNUC_UNUSED, GList **imgs G_GNUC_UNUSED)
{
  int *calls = item;
  (*calls)++;
}

static void test_clear_grouping_undo_preserves_unrelated_undo_and_redo(void **state G_GNUC_UNUSED)
{
  dt_undo_t *const undo = dt_undo_init();
  int ratings_undo = 0;
  int grouping_undo = 0;

  dt_undo_record(undo, NULL, DT_UNDO_RATINGS, &ratings_undo, _count_undo_calls, NULL);
  dt_undo_record(undo, NULL, DT_UNDO_GROUPING, &grouping_undo, _count_undo_calls, NULL);
  dt_undo_do_undo(undo, DT_UNDO_GROUPING);
  dt_undo_do_redo(undo, DT_UNDO_GROUPING);
  dt_undo_do_undo(undo, DT_UNDO_RATINGS);
  assert_int_equal(dt_undo_list_length(undo, DT_UNDO_GROUPING), 1);
  assert_int_equal(dt_redo_list_length(undo, DT_UNDO_RATINGS), 1);

  dt_undo_clear(undo, DT_UNDO_GROUPING);

  assert_int_equal(dt_redo_list_length(undo, DT_UNDO_RATINGS), 1);
  assert_int_equal(dt_undo_list_length(undo, DT_UNDO_GROUPING), 0);
  assert_int_equal(dt_redo_list_length(undo, DT_UNDO_GROUPING), 0);
  dt_undo_cleanup(undo);
}

static void test_auto_group_undo_redo_restores_the_complete_assignment_map(void **state G_GNUC_UNUSED)
{
  dt_control_t control;
  dt_image_group_assignment_t assignments[2];
  _create_pair_fixture("/testdb/auto-group-undo", assignments);
  const int32_t leader = assignments[0].imgid;
  const int32_t member = assignments[1].imgid;
  const dt_auto_group_plan_t plan = { .assignments = assignments,
                                      .count = G_N_ELEMENTS(assignments) };

  _start_gui_services(&control);
  darktable.undo = dt_undo_init();
  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_OK);
  assert_true(dt_is_undo_list_populated(darktable.undo, DT_UNDO_GROUPING));
  dt_undo_do_undo(darktable.undo, DT_UNDO_GROUPING);
  GList *members = dt_image_repository_get_group_members(member, -1);
  assert_int_equal(g_list_length(members), 1);
  g_list_free(members);
  dt_undo_do_redo(darktable.undo, DT_UNDO_GROUPING);
  members = dt_image_repository_get_group_members(leader, -1);
  assert_int_equal(g_list_length(members), 2);
  g_list_free(members);
  dt_undo_cleanup(darktable.undo);
  darktable.undo = NULL;
  _stop_gui_services(&control);
}

static void test_auto_group_undo_does_not_batch_with_a_preceding_rating(void **state G_GNUC_UNUSED)
{
  dt_control_t control;
  dt_image_group_assignment_t assignments[2];
  _create_pair_fixture("/testdb/auto-group-undo-isolation", assignments);
  const int32_t member = assignments[1].imgid;
  const dt_auto_group_plan_t plan = { .assignments = assignments,
                                      .count = G_N_ELEMENTS(assignments) };
  int rating_undo = 0;

  _start_gui_services(&control);
  darktable.undo = dt_undo_init();
  dt_undo_record(darktable.undo, NULL, DT_UNDO_RATINGS, &rating_undo, _count_undo_calls, NULL);
  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_OK);

  dt_undo_do_undo(darktable.undo, DT_UNDO_LIGHTTABLE);

  GList *const members = dt_image_repository_get_group_members(member, -1);
  assert_int_equal(g_list_length(members), 1);
  g_list_free(members);
  assert_true(dt_is_undo_list_populated(darktable.undo, DT_UNDO_RATINGS));
  dt_undo_cleanup(darktable.undo);
  darktable.undo = NULL;
  _stop_gui_services(&control);
}

static void test_auto_group_apply_refresh_undo_redo_then_manual_mutation_invalidates_history(void **state G_GNUC_UNUSED)
{
  dt_control_t control;
  dt_auto_group_notifications_t notifications = { 0 };
  dt_image_group_assignment_t assignments[2];
  _create_pair_fixture("/testdb/auto-group-workflow", assignments);
  const int32_t member = assignments[1].imgid;
  const dt_auto_group_plan_t plan = { .assignments = assignments,
                                      .count = G_N_ELEMENTS(assignments) };

  _start_gui_services(&control);
  darktable.undo = dt_undo_init();
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, G_CALLBACK(_image_info_changed),
                            &notifications);

  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_OK);
  _dispatch_pending_events();
  assert_int_equal(notifications.image_info_changed, 1);
  dt_undo_do_undo(darktable.undo, DT_UNDO_GROUPING);
  dt_undo_do_redo(darktable.undo, DT_UNDO_GROUPING);
  dt_grouping_remove_from_group(member);

  assert_false(dt_is_undo_list_populated(darktable.undo, DT_UNDO_GROUPING));
  assert_false(dt_is_redo_list_populated(darktable.undo, DT_UNDO_GROUPING));
  g_list_free(notifications.imgids);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_image_info_changed), &notifications);
  dt_undo_cleanup(darktable.undo);
  darktable.undo = NULL;
  _stop_gui_services(&control);
}

static void test_auto_group_plans_and_executes_ten_thousand_deterministic_candidates(void **state G_GNUC_UNUSED)
{
  enum { candidate_count = 10000 };
  dt_control_t control;
  const int32_t film_id = testdb_make_film("/testdb/auto-group-ten-thousand");
  dt_auto_group_snapshot_t snapshot = { .images = g_new0(dt_auto_group_image_t, candidate_count),
                                        .count = candidate_count };
  dt_auto_group_plan_t plan = { 0 };

  for(size_t index = 0; index < candidate_count; index++)
  {
    char filename[32] = { 0 };
    g_snprintf(filename, sizeof(filename), "candidate-%05" G_GSIZE_FORMAT ".raw", index);
    const int32_t imgid = testdb_make_image(film_id, filename);
    assert_true(dt_image_repository_set_group(imgid, imgid));
    snapshot.images[index] = (dt_auto_group_image_t){ .imgid = imgid,
                                                        .group_id = imgid,
                                                        .film_id = film_id,
                                                        .group_members = 1,
                                                        .datetime_taken = (int64_t)(index / 2 + 1) * G_USEC_PER_SEC };
    g_strlcpy(snapshot.images[index].camera_maker, "Ansel", sizeof(snapshot.images[index].camera_maker));
    g_strlcpy(snapshot.images[index].camera_model, "fixture", sizeof(snapshot.images[index].camera_model));
  }

  const gint64 plan_started = g_get_monotonic_time();
  assert_int_equal(dt_auto_group_plan(&snapshot, 0, &plan), DT_AUTO_GROUP_STATUS_OK);
  const gint64 plan_elapsed = g_get_monotonic_time() - plan_started;
  _start_gui_services(&control);
  const gint64 execution_started = g_get_monotonic_time();
  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_OK);
  const gint64 execution_elapsed = g_get_monotonic_time() - execution_started;
  _dispatch_pending_events();

  assert_int_equal(plan.count, candidate_count);
  print_message("10k automatic grouping: plan=%" G_GINT64_FORMAT " us, execute=%" G_GINT64_FORMAT " us, assignments=%" G_GSIZE_FORMAT "\n",
                plan_elapsed, execution_elapsed, plan.count);
  dt_auto_group_plan_cleanup(&plan);
  dt_auto_group_snapshot_cleanup(&snapshot);
  _stop_gui_services(&control);
}

static void test_grouping_invalidation_clears_both_history_directions(void **state G_GNUC_UNUSED)
{
  darktable.undo = dt_undo_init();
  int undo_data = 0;
  dt_undo_record(darktable.undo, NULL, DT_UNDO_GROUPING, &undo_data, _count_undo_calls, NULL);
  dt_undo_do_undo(darktable.undo, DT_UNDO_GROUPING);

  dt_undo_clear(darktable.undo, DT_UNDO_GROUPING);

  assert_false(dt_is_undo_list_populated(darktable.undo, DT_UNDO_GROUPING));
  assert_false(dt_is_redo_list_populated(darktable.undo, DT_UNDO_GROUPING));
  dt_undo_cleanup(darktable.undo);
  darktable.undo = NULL;
}

static void test_auto_group_undo_failure_rolls_back_and_discards_stale_history(void **state G_GNUC_UNUSED)
{
  dt_control_t control;
  dt_image_group_assignment_t assignments[2];
  _create_pair_fixture("/testdb/auto-group-undo-failure", assignments);
  const int32_t leader = assignments[0].imgid;
  const dt_auto_group_plan_t plan = { .assignments = assignments,
                                      .count = G_N_ELEMENTS(assignments) };
  sqlite3 *const database = dt_database_get_sqlite3_global();

  _start_gui_services(&control);
  darktable.undo = dt_undo_init();
  assert_int_equal(dt_auto_group_execute(&plan), DT_AUTO_GROUP_STATUS_OK);
  assert_int_equal(sqlite3_set_authorizer(database, _deny_transaction, "BEGIN"), SQLITE_OK);
  dt_undo_do_undo(darktable.undo, DT_UNDO_GROUPING);
  assert_int_equal(sqlite3_set_authorizer(database, NULL, NULL), SQLITE_OK);
  assert_int_equal(_restore_failures, 1);
  GList *const members = dt_image_repository_get_group_members(leader, -1);
  assert_int_equal(g_list_length(members), 2);
  g_list_free(members);
  assert_false(dt_is_undo_list_populated(darktable.undo, DT_UNDO_GROUPING));
  assert_false(dt_is_redo_list_populated(darktable.undo, DT_UNDO_GROUPING));
  dt_undo_cleanup(darktable.undo);
  darktable.undo = NULL;
  _stop_gui_services(&control);
}

int main(int argc, char *argv[])
{
  dt_datetime_init();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_new_image_starts_in_its_own_group),
    cmocka_unit_test(test_plan_groups_unordered_consecutive_matching_images),
    cmocka_unit_test(test_plan_skips_existing_groups_missing_timestamps_and_singletons),
    cmocka_unit_test(test_plan_zero_interval_groups_only_exact_microsecond_timestamps),
    cmocka_unit_test(test_plan_rejects_an_out_of_range_interval),
    cmocka_unit_test(test_snapshot_discards_rows_when_collection_read_fails),
    cmocka_unit_test(test_snapshot_normalizes_camera_identity_through_rawspeed),
    cmocka_unit_test(test_execute_commits_batch_and_notifies_once),
    cmocka_unit_test(test_execute_rolls_back_late_conflict_without_notifications),
    cmocka_unit_test(test_execute_empty_plan_does_not_write_or_notify),
    cmocka_unit_test(test_execute_suppresses_notifications_when_begin_fails),
    cmocka_unit_test(test_execute_rolls_back_and_suppresses_notifications_when_commit_fails),
    cmocka_unit_test(test_clear_grouping_undo_preserves_unrelated_undo_and_redo),
    cmocka_unit_test(test_auto_group_undo_redo_restores_the_complete_assignment_map),
    cmocka_unit_test(test_auto_group_undo_does_not_batch_with_a_preceding_rating),
    cmocka_unit_test(test_auto_group_apply_refresh_undo_redo_then_manual_mutation_invalidates_history),
    cmocka_unit_test(test_grouping_invalidation_clears_both_history_directions),
    cmocka_unit_test(test_auto_group_undo_failure_rolls_back_and_discards_stale_history),
    cmocka_unit_test(test_auto_group_plans_and_executes_ten_thousand_deterministic_candidates),
  };
  const int result = cmocka_run_group_tests(tests, _setup, _teardown);
  dt_image_notify_set_changed_handler(NULL);
  dt_auto_group_set_message_handler(NULL);
  dt_control_signal_cleanup(darktable.signals);
  darktable.signals = NULL;
  return result;
}
