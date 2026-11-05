#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>
#include <glib.h>

#include "common/logging.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define dt_vprint(thread, format, ...) dt_test_debug_trace_log((thread), (format))
#include "common/debug.h"

enum dt_test_debug_trace_event_t
{
  DT_TEST_DEBUG_TRACE_LOG,
  DT_TEST_DEBUG_TRACE_CALL
};

static enum dt_test_debug_trace_event_t trace_events[2];
static int trace_event_count;
static int trace_call_count;

void dt_test_debug_trace_log(const dt_debug_thread_t thread, const char *const format)
{
  assert_int_equal(thread, DT_DEBUG_SQL);
  assert_string_equal(format, "[debug_trace] %s is called from %s at %s:%d\n");
  assert_true(trace_event_count < 2);
  trace_events[trace_event_count++] = DT_TEST_DEBUG_TRACE_LOG;
}

static gboolean trace_boolean_result(void)
{
  assert_true(trace_event_count < 2);
  trace_events[trace_event_count++] = DT_TEST_DEBUG_TRACE_CALL;
  trace_call_count++;
  return TRUE;
}

#ifndef DT_DEBUG_TRACE_WRAPPER_RESULT_NO_ARGS
#error "DT_DEBUG_TRACE_WRAPPER_RESULT_NO_ARGS must be defined"
#endif

static void test_no_argument_boolean_result_is_logged_then_called_once(void **state G_GNUC_UNUSED)
{
  trace_event_count = 0;
  trace_call_count = 0;

  assert_true(_Generic(DT_DEBUG_TRACE_WRAPPER_RESULT_NO_ARGS(DT_DEBUG_SQL, trace_boolean_result),
                       gboolean : TRUE,
                       default : FALSE));
  assert_true(DT_DEBUG_TRACE_WRAPPER_RESULT_NO_ARGS(DT_DEBUG_SQL, trace_boolean_result));
  assert_int_equal(trace_call_count, 1);
  assert_int_equal(trace_event_count, 2);
  assert_int_equal(trace_events[0], DT_TEST_DEBUG_TRACE_LOG);
  assert_int_equal(trace_events[1], DT_TEST_DEBUG_TRACE_CALL);
}

#undef dt_vprint

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_no_argument_boolean_result_is_logged_then_called_once),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
