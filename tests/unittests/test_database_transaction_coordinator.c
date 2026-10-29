/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "testdb.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static gboolean fail_begin = FALSE;
static gboolean fail_commit = FALSE;
static gboolean fail_savepoint_release = FALSE;
static gboolean fail_module_order_delete = FALSE;
static gboolean unavailable_mutex = FALSE;
static int rollback_count = 0;
static int connection_mutex_depth = 0;
static gboolean invalid_mutex_lifetime = FALSE;
static gboolean coordinate_repository_read = FALSE;
static pthread_t coordinating_reader;
static pthread_mutex_t coordination_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t coordination_condition = PTHREAD_COND_INITIALIZER;
static gboolean reader_reached_connection = FALSE;

typedef int (*sqlite3_exec_t)(sqlite3 *, const char *, sqlite3_callback, void *, char **);
typedef int (*sqlite3_step_t)(sqlite3_stmt *);
typedef sqlite3_mutex *(*sqlite3_db_mutex_t)(sqlite3 *);
typedef void (*sqlite3_mutex_t)(sqlite3_mutex *);

static sqlite3_exec_t real_sqlite3_exec = NULL;
static sqlite3_step_t real_sqlite3_step = NULL;
static sqlite3_db_mutex_t real_sqlite3_db_mutex = NULL;
static sqlite3_mutex_t real_sqlite3_mutex_enter = NULL;
static sqlite3_mutex_t real_sqlite3_mutex_leave = NULL;

int sqlite3_exec(sqlite3 *db, const char *sql, sqlite3_callback callback, void *data, char **error)
{
  if(!strcmp(sql, "BEGIN TRANSACTION") || !strcmp(sql, "COMMIT TRANSACTION") || !strcmp(sql, "ROLLBACK TRANSACTION"))
    invalid_mutex_lifetime = invalid_mutex_lifetime || connection_mutex_depth != 1;
  if(!strcmp(sql, "BEGIN TRANSACTION") && fail_begin) return SQLITE_ERROR;
  if(!strcmp(sql, "COMMIT TRANSACTION") && fail_commit) return SQLITE_ERROR;
  if(fail_savepoint_release && g_str_has_prefix(sql, "RELEASE SAVEPOINT trx")) return SQLITE_ERROR;
  if(!strcmp(sql, "ROLLBACK TRANSACTION")) rollback_count++;
  return real_sqlite3_exec(db, sql, callback, data, error);
}

int sqlite3_step(sqlite3_stmt *statement)
{
  if(fail_module_order_delete
     && !strcmp(sqlite3_sql(statement), "DELETE FROM main.module_order WHERE imgid = ?1"))
    return SQLITE_ERROR;
  return real_sqlite3_step(statement);
}

sqlite3_mutex *sqlite3_db_mutex(sqlite3 *db)
{
  if(IS_NULL_PTR(real_sqlite3_db_mutex)) real_sqlite3_db_mutex = (sqlite3_db_mutex_t)dlsym(RTLD_NEXT, "sqlite3_db_mutex");
  if(unavailable_mutex) return NULL;
  return real_sqlite3_db_mutex(db);
}

void sqlite3_mutex_enter(sqlite3_mutex *mutex)
{
  if(IS_NULL_PTR(real_sqlite3_mutex_enter)) real_sqlite3_mutex_enter = (sqlite3_mutex_t)dlsym(RTLD_NEXT, "sqlite3_mutex_enter");
  if(coordinate_repository_read && pthread_equal(pthread_self(), coordinating_reader)
     && mutex == real_sqlite3_db_mutex(dt_database_get_sqlite3_global()))
  {
    pthread_mutex_lock(&coordination_mutex);
    if(!reader_reached_connection)
    {
      reader_reached_connection = TRUE;
      pthread_cond_signal(&coordination_condition);
    }
    pthread_mutex_unlock(&coordination_mutex);
  }
  real_sqlite3_mutex_enter(mutex);
  connection_mutex_depth++;
}

void sqlite3_mutex_leave(sqlite3_mutex *mutex)
{
  if(IS_NULL_PTR(real_sqlite3_mutex_leave)) real_sqlite3_mutex_leave = (sqlite3_mutex_t)dlsym(RTLD_NEXT, "sqlite3_mutex_leave");
  real_sqlite3_mutex_leave(mutex);
  connection_mutex_depth--;
}

static void reset_faults(void)
{
  fail_begin = FALSE;
  fail_commit = FALSE;
  fail_savepoint_release = FALSE;
  fail_module_order_delete = FALSE;
  unavailable_mutex = FALSE;
  rollback_count = 0;
  connection_mutex_depth = 0;
  invalid_mutex_lifetime = FALSE;
  coordinate_repository_read = FALSE;
  reader_reached_connection = FALSE;
}

static int coordinator_setup(void **state)
{
  real_sqlite3_exec = (sqlite3_exec_t)dlsym(RTLD_NEXT, "sqlite3_exec");
  real_sqlite3_step = (sqlite3_step_t)dlsym(RTLD_NEXT, "sqlite3_step");
  if(IS_NULL_PTR(real_sqlite3_exec) || IS_NULL_PTR(real_sqlite3_step)) return -1;
  reset_faults();
  return testdb_setup(state);
}

static void assert_fresh_transaction(void)
{
  assert_true(dt_database_start_transaction());
  assert_true(dt_database_release_transaction());
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
}

static void test_begin_failure_cleans_up(void **state)
{
  (void)state;
  reset_faults();
  fail_begin = TRUE;
  assert_false(dt_database_start_transaction());
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
  reset_faults();
  assert_fresh_transaction();
}

static void test_unavailable_mutex_cleans_up(void **state)
{
  (void)state;
  reset_faults();
  unavailable_mutex = TRUE;
  assert_false(dt_database_start_transaction());
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
  reset_faults();
  assert_fresh_transaction();
}

static void test_batch_start_failures_clean_up(void **state)
{
  (void)state;
  reset_faults();
  fail_begin = TRUE;
  dt_database_begin_transaction_batch();
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
  reset_faults();
  unavailable_mutex = TRUE;
  dt_database_begin_transaction_batch();
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  reset_faults();
  assert_fresh_transaction();
}

static void test_commit_failure_rolls_back_once(void **state)
{
  (void)state;
  reset_faults();
  assert_true(dt_database_start_transaction());
  fail_commit = TRUE;
  assert_false(dt_database_release_transaction());
  assert_int_equal(rollback_count, 1);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
  reset_faults();
  assert_fresh_transaction();
}

static void test_owner_rollback_cleans_up(void **state)
{
  (void)state;
  reset_faults();
  assert_true(dt_database_start_transaction());
  dt_database_rollback_transaction();
  assert_int_equal(rollback_count, 1);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
  assert_fresh_transaction();
}

static void test_history_delete_rolls_back_partial_failure(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/history-delete-rollback");
  const int32_t image = testdb_make_image(film, "rollback.raw");
  const uint8_t params[] = { 1 };
  const uint8_t blend[] = { 2 };
  assert_true(image > 0);
  assert_true(dt_history_repository_write_item(image, 0, "exposure", params, sizeof(params), 1,
                                               TRUE, blend, sizeof(blend), 1, 0, ""));
  assert_true(dt_history_repository_set_end(image, 1));

  fail_module_order_delete = TRUE;
  assert_false(dt_history_repository_delete_all_for_image(image));
  fail_module_order_delete = FALSE;

  assert_int_equal(dt_history_repository_count_items(image), 1);
  assert_int_equal(dt_history_repository_get_end(image), 1);
  assert_fresh_transaction();

  const int32_t sentinel = testdb_make_image(film, "sentinel.raw");
  const int64_t original_timestamp = dt_image_repository_get_write_timestamp(sentinel);
  assert_true(sentinel > 0);
  assert_true(dt_database_start_transaction());
  assert_true(dt_image_repository_set_write_timestamp(sentinel, 42));
  fail_module_order_delete = TRUE;
  assert_false(dt_history_repository_delete_all_for_image(image));
  fail_module_order_delete = FALSE;
  assert_true(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(dt_image_repository_get_write_timestamp(sentinel), 42);
  assert_true(dt_database_release_transaction());
  assert_int_equal(dt_image_repository_get_write_timestamp(sentinel), 42);
  assert_int_equal(dt_history_repository_count_items(image), 1);
  assert_int_equal(dt_history_repository_get_end(image), 1);

  assert_true(dt_database_start_transaction());
  assert_true(dt_image_repository_set_write_timestamp(sentinel, 84));
  fail_module_order_delete = TRUE;
  assert_false(dt_history_repository_delete_all_for_image(image));
  fail_module_order_delete = FALSE;
  dt_database_rollback_transaction();
  assert_int_equal(dt_image_repository_get_write_timestamp(sentinel), 42);
  assert_int_equal(dt_history_repository_count_items(image), 1);
  assert_int_equal(dt_history_repository_get_end(image), 1);

  dt_database_begin_transaction_batch();
  assert_true(dt_image_repository_set_write_timestamp(sentinel, 126));
  fail_module_order_delete = TRUE;
  assert_false(dt_history_repository_delete_all_for_image(image));
  fail_module_order_delete = FALSE;
  assert_true(dt_database_transaction_is_owned_by_current_thread());
  dt_database_end_transaction_batch();
  assert_int_equal(dt_image_repository_get_write_timestamp(sentinel), 126);
  assert_int_equal(dt_history_repository_count_items(image), 1);
  assert_int_equal(dt_history_repository_get_end(image), 1);
  assert_int_not_equal(original_timestamp, 126);
}

static void test_batch_commit_failure_rolls_back_once(void **state)
{
  (void)state;
  reset_faults();
  dt_database_begin_transaction_batch();
  fail_commit = TRUE;
  dt_database_end_transaction_batch();
  assert_int_equal(rollback_count, 1);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
  reset_faults();
  assert_fresh_transaction();
}

static void test_same_thread_nesting_and_batch(void **state)
{
  (void)state;
  reset_faults();
  assert_true(dt_database_start_transaction());
  assert_true(dt_database_start_transaction());
  assert_true(dt_database_release_transaction());
  assert_true(dt_database_release_transaction());
  assert_true(dt_database_start_transaction());
  dt_database_begin_transaction_batch();
  assert_true(dt_database_start_transaction());
  assert_true(dt_database_release_transaction());
  dt_database_end_transaction_batch();
  assert_true(dt_database_release_transaction());
  dt_database_begin_transaction_batch();
  assert_true(dt_database_start_transaction());
  assert_true(dt_database_release_transaction());
  dt_database_end_transaction_batch();
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
  assert_fresh_transaction();
}

static void test_nested_rollback_preserves_outer_transaction(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/nested-savepoint");
  const int32_t outer_image = testdb_make_image(film, "outer.raw");
  const int32_t nested_image = testdb_make_image(film, "nested.raw");
  const int64_t original_nested_timestamp = dt_image_repository_get_write_timestamp(nested_image);

  assert_true(dt_database_start_transaction());
  assert_true(dt_image_repository_set_write_timestamp(outer_image, 101));
  assert_true(dt_database_start_transaction());
  assert_true(dt_image_repository_set_write_timestamp(nested_image, 202));
  dt_database_rollback_transaction();

  assert_true(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(dt_image_repository_get_write_timestamp(outer_image), 101);
  assert_int_equal(dt_image_repository_get_write_timestamp(nested_image), original_nested_timestamp);

  assert_true(dt_database_start_transaction());
  assert_true(dt_image_repository_set_write_timestamp(nested_image, 303));
  assert_true(dt_database_release_transaction());
  assert_true(dt_database_release_transaction());

  assert_int_equal(dt_image_repository_get_write_timestamp(outer_image), 101);
  assert_int_equal(dt_image_repository_get_write_timestamp(nested_image), 303);
  assert_fresh_transaction();
}

static void test_nested_batch_release_failure_allows_retry(void **state)
{
  (void)state;
  reset_faults();
  assert_true(dt_database_start_transaction());
  dt_database_begin_transaction_batch();
  fail_savepoint_release = TRUE;

  dt_database_end_transaction_batch();
  assert_true(dt_database_transaction_is_owned_by_current_thread());

  fail_savepoint_release = FALSE;
  dt_database_end_transaction_batch();
  assert_true(dt_database_transaction_is_owned_by_current_thread());
  assert_true(dt_database_release_transaction());
  assert_fresh_transaction();
}

static void test_nested_batch_release_failure_allows_rollback(void **state)
{
  (void)state;
  reset_faults();
  assert_true(dt_database_start_transaction());
  dt_database_begin_transaction_batch();
  fail_savepoint_release = TRUE;

  dt_database_end_transaction_batch();
  assert_true(dt_database_transaction_is_owned_by_current_thread());

  dt_database_rollback_transaction();
  assert_true(dt_database_transaction_is_owned_by_current_thread());
  fail_savepoint_release = FALSE;
  assert_true(dt_database_release_transaction());
  assert_fresh_transaction();
}

typedef struct concurrent_access_t
{
  int32_t image;
  gboolean read_finished;
} concurrent_access_t;

static void *read_while_transaction_owns_connection(void *user_data)
{
  concurrent_access_t *access = user_data;
  coordinating_reader = pthread_self();
  dt_image_repository_get_write_timestamp(access->image);
  access->read_finished = TRUE;
  return NULL;
}

static int run_transaction_owner_repository_read_race(void)
{
  const int32_t film = testdb_make_film("/testdb/repository-read-transaction-race");
  concurrent_access_t access = { .image = testdb_make_image(film, "race.raw"), .read_finished = FALSE };
  pthread_t reader;
  dt_image_repository_get_write_timestamp(access.image);
  if(!dt_database_start_transaction()) return 1;
  coordinate_repository_read = TRUE;
  if(pthread_create(&reader, NULL, read_while_transaction_owns_connection, &access) != 0) return 1;

  pthread_mutex_lock(&coordination_mutex);
  while(!reader_reached_connection)
    pthread_cond_wait(&coordination_condition, &coordination_mutex);
  pthread_mutex_unlock(&coordination_mutex);

  dt_image_repository_get_write_timestamp(access.image);
  if(!dt_database_release_transaction()) return 1;
  if(pthread_join(reader, NULL) != 0) return 1;
  return access.read_finished ? 0 : 1;
}

static void test_transaction_owner_and_repository_reader_do_not_deadlock(void **state)
{
  (void)state;
  const pid_t child = fork();
  assert_true(child >= 0);
  if(child == 0) _exit(run_transaction_owner_repository_read_race());

  int status = 0;
  for(int attempt = 0; attempt < 100; attempt++)
  {
    const pid_t result = waitpid(child, &status, WNOHANG);
    if(result == child)
    {
      assert_true(WIFEXITED(status));
      assert_int_equal(WEXITSTATUS(status), 0);
      return;
    }
    assert_int_equal(result, 0);
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&delay, NULL);
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  fail_msg("transaction owner and repository reader did not complete within one second");
}

typedef struct transaction_barrier_t
{
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  gboolean owner_started;
  gboolean non_owner_finished;
} transaction_barrier_t;

static void *attempt_non_owner_transitions(void *user_data)
{
  transaction_barrier_t *barrier = user_data;
  pthread_mutex_lock(&barrier->mutex);
  while(!barrier->owner_started)
    pthread_cond_wait(&barrier->condition, &barrier->mutex);
  pthread_mutex_unlock(&barrier->mutex);

  assert_false(dt_database_release_transaction());
  dt_database_rollback_transaction();

  pthread_mutex_lock(&barrier->mutex);
  barrier->non_owner_finished = TRUE;
  pthread_cond_signal(&barrier->condition);
  pthread_mutex_unlock(&barrier->mutex);
  return NULL;
}

static void test_non_owner_transitions_preserve_owner(void **state)
{
  (void)state;
  reset_faults();
  transaction_barrier_t barrier = { .mutex = PTHREAD_MUTEX_INITIALIZER,
                                    .condition = PTHREAD_COND_INITIALIZER,
                                    .owner_started = FALSE,
                                    .non_owner_finished = FALSE };
  pthread_t thread;

  assert_true(dt_database_start_transaction());
  assert_int_equal(pthread_create(&thread, NULL, attempt_non_owner_transitions, &barrier), 0);

  pthread_mutex_lock(&barrier.mutex);
  barrier.owner_started = TRUE;
  pthread_cond_signal(&barrier.condition);
  while(!barrier.non_owner_finished)
    pthread_cond_wait(&barrier.condition, &barrier.mutex);
  pthread_mutex_unlock(&barrier.mutex);

  assert_true(dt_database_transaction_is_owned_by_current_thread());
  assert_true(dt_database_release_transaction());
  assert_int_equal(pthread_join(thread, NULL), 0);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  assert_int_equal(connection_mutex_depth, 0);
  assert_false(invalid_mutex_lifetime);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_begin_failure_cleans_up),
    cmocka_unit_test(test_unavailable_mutex_cleans_up),
    cmocka_unit_test(test_batch_start_failures_clean_up),
    cmocka_unit_test(test_commit_failure_rolls_back_once),
    cmocka_unit_test(test_owner_rollback_cleans_up),
    cmocka_unit_test(test_history_delete_rolls_back_partial_failure),
    cmocka_unit_test(test_batch_commit_failure_rolls_back_once),
    cmocka_unit_test(test_same_thread_nesting_and_batch),
    cmocka_unit_test(test_nested_rollback_preserves_outer_transaction),
    cmocka_unit_test(test_nested_batch_release_failure_allows_retry),
    cmocka_unit_test(test_nested_batch_release_failure_allows_rollback),
    cmocka_unit_test(test_transaction_owner_and_repository_reader_do_not_deadlock),
    cmocka_unit_test(test_non_owner_transitions_preserve_owner),
  };
  return cmocka_run_group_tests(tests, coordinator_setup, testdb_teardown);
}
