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

/** The inversion that makes src/metadata a closed module: it states what happened, and
 * whoever is running it decides what that looks like.
 *
 * These tests exist because the no-handler case is the one nothing else exercises -- every
 * interactive run installs a handler in dt_init(), so a crash or a swallowed message there
 * would only ever show up under ansel-cli or in a test binary. Which is to say: here.
 */

#include "metadata/notify.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope and does not
// include <setjmp.h> itself -- it says so at its own line 50. No test names a setjmp
// symbol, so include-cleaner cannot attribute this one; removing it stops cmocka.h from
// parsing. The suppression is on the line itself, not NOLINTNEXTLINE above: that form
// applies to the immediately following line, which a multi-line reason turns into another
// comment, silently suppressing nothing.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <glib.h>

typedef struct _seen_t
{
  int toasts;
  int messages;
  int tag_changes;
  gchar *last;
} _seen_t;

static _seen_t _seen;

static void _record(const dt_metadata_notice_t kind, const char *message)
{
  if(kind == DT_METADATA_NOTICE_TOAST) _seen.toasts++;
  else _seen.messages++;
  g_free(_seen.last);
  _seen.last = g_strdup(message);
}

static void _record_tag_change(void)
{
  _seen.tag_changes++;
}

static int _setup(void **state)
{
  (void)state;
  memset(&_seen, 0, sizeof(_seen));
  return 0;
}

static int _teardown(void **state)
{
  (void)state;
  dt_metadata_set_notify_handler(NULL);
  dt_metadata_set_tags_changed_handler(NULL);
  g_free(_seen.last);
  _seen.last = NULL;
  return 0;
}

/* The headless contract: with nothing installed, raising a message must be a silent no-op
 * rather than a crash. ansel-cli and every unit test run in exactly this state. */
static void test_no_handler_is_silent(void **state)
{
  (void)state;
  dt_metadata_set_notify_handler(NULL);
  dt_metadata_set_tags_changed_handler(NULL);

  dt_metadata_notify(DT_METADATA_NOTICE_TOAST, "%s", "dropped");
  dt_metadata_notify(DT_METADATA_NOTICE_MESSAGE, "dropped %d", 1);
  dt_metadata_tags_changed();

  assert_int_equal(_seen.toasts, 0);
  assert_int_equal(_seen.messages, 0);
  assert_int_equal(_seen.tag_changes, 0);
}

/* The two notice kinds are not interchangeable: one is a transient acknowledgement, the
 * other the running commentary, and they land in different widgets. */
static void test_kinds_stay_distinct(void **state)
{
  (void)state;
  dt_metadata_set_notify_handler(_record);

  dt_metadata_notify(DT_METADATA_NOTICE_TOAST, "Rating set to %s for %i image(s)", "three", 12);
  assert_int_equal(_seen.toasts, 1);
  assert_int_equal(_seen.messages, 0);
  assert_string_equal(_seen.last, "Rating set to three for 12 image(s)");

  dt_metadata_notify(DT_METADATA_NOTICE_MESSAGE, "no images selected to apply rating");
  assert_int_equal(_seen.toasts, 1);
  assert_int_equal(_seen.messages, 1);
  assert_string_equal(_seen.last, "no images selected to apply rating");
}

/* Removing the handler must actually remove it -- a stale pointer here would outlive the
 * GUI it belongs to. */
static void test_handler_can_be_removed(void **state)
{
  (void)state;
  dt_metadata_set_notify_handler(_record);
  dt_metadata_notify(DT_METADATA_NOTICE_MESSAGE, "heard");
  assert_int_equal(_seen.messages, 1);

  dt_metadata_set_notify_handler(NULL);
  dt_metadata_notify(DT_METADATA_NOTICE_MESSAGE, "not heard");
  assert_int_equal(_seen.messages, 1);
  assert_string_equal(_seen.last, "heard");
}

/* The tag-changed notification carries no payload on purpose: every consumer re-reads. */
static void test_tags_changed_fires_each_time(void **state)
{
  (void)state;
  dt_metadata_set_tags_changed_handler(_record_tag_change);

  dt_metadata_tags_changed();
  dt_metadata_tags_changed();
  assert_int_equal(_seen.tag_changes, 2);

  dt_metadata_set_tags_changed_handler(NULL);
  dt_metadata_tags_changed();
  assert_int_equal(_seen.tag_changes, 2);
}

/* The two channels are independent: installing one must not arm or silence the other. */
static void test_channels_are_independent(void **state)
{
  (void)state;
  dt_metadata_set_notify_handler(_record);

  dt_metadata_tags_changed(); // no tags handler yet
  assert_int_equal(_seen.tag_changes, 0);

  dt_metadata_set_tags_changed_handler(_record_tag_change);
  dt_metadata_set_notify_handler(NULL);

  dt_metadata_notify(DT_METADATA_NOTICE_TOAST, "dropped");
  dt_metadata_tags_changed();
  assert_int_equal(_seen.toasts, 0);
  assert_int_equal(_seen.tag_changes, 1);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_no_handler_is_silent, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_kinds_stay_distinct, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_handler_can_be_removed, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_tags_changed_fires_each_time, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_channels_are_independent, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
