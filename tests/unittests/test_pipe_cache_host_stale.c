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

/** A cacheline rekeyed in place publishes no host pixels until they are rewritten.
 *
 * A module's output cacheline is reused for its next output by moving it to the new hash, host
 * buffer included. When that next output stays on the GPU, the host buffer keeps the previous
 * hash's pixels under the new key, where a CPU-only module downstream, the GUI or an exact hit in
 * another pipe would read them as the new hash's.
 *
 * These tests drive the cache alone, without a pipe or a device: the contract is the cache's.
 */

#include "caches/pixelpipe_cache.h"
#include "system/atomic.h"
#include "system/dtpthread.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_metadata_notify.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#define LINE_BYTES (256 * 1024)
#define HASH_A 0x0A0A0A0A0A0A0A0Aull
#define HASH_B 0x0B0B0B0B0B0B0B0Bull
#define HASH_C 0x0C0C0C0C0C0C0C0Cull

static int _setup(void **state __attribute__((unused)))
{
  return dt_dev_pixelpipe_cache_init((size_t)64 * 1024 * 1024, FALSE, FALSE) ? 0 : -1;
}

static int _teardown(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_cache_cleanup();
  return 0;
}

/* Take a writable line the way the pipeline does, fill its host buffer if asked, and publish it:
 * write lock and reference released. Returns the entry's metadata snapshot, which is what a piece
 * keeps as its reuse hint. */
static dt_pixel_cache_entry_t _produce(const uint64_t hash, const dt_pixel_cache_entry_t *hint,
                                       const gboolean allow_rekey, const gboolean write_host,
                                       const dt_dev_pixelpipe_cache_writable_status_t expected)
{
  void *data = NULL;
  dt_pixel_cache_entry_t *entry = NULL;
  const dt_dev_pixelpipe_cache_writable_status_t status
      = dt_dev_pixelpipe_cache_get_writable(hash, LINE_BYTES, "test line", 0, write_host, allow_rekey,
                                            hint, NULL, &data, &entry);
  assert_int_equal(status, expected);
  assert_non_null(entry);

  if(write_host)
  {
    void *buffer = dt_pixel_cache_alloc(entry);
    assert_non_null(buffer);
    memset(buffer, (int)(hash & 0xff), LINE_BYTES);
    dt_dev_pixelpipe_cache_flag_host_written(entry);
  }

  const dt_pixel_cache_entry_t snapshot = *entry;
  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
  return snapshot;
}

/** An output kept on the device after a rekey: the host bytes are the previous hash's. */
static void _rekeyed_line_publishes_no_host_pixels(void **state __attribute__((unused)))
{
  const dt_pixel_cache_entry_t first
      = _produce(HASH_A, NULL, TRUE, TRUE, DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED);
  const dt_pixel_cache_entry_t second
      = _produce(HASH_B, &first, TRUE, FALSE, DT_DEV_PIXELPIPE_CACHE_WRITABLE_REKEYED);

  // Same line, moved: the rekey is what this is about.
  assert_int_equal(second.serial, first.serial);
  assert_null(dt_dev_pixelpipe_cache_get_entry(HASH_A));

  dt_pixel_cache_entry_t *entry = dt_dev_pixelpipe_cache_get_entry(HASH_B);
  assert_non_null(entry);
  // The producer still reaches the buffer it backs its output with ...
  assert_ptr_equal(dt_pixel_cache_entry_get_buffer(entry), first.data);
  // ... and no reader sees it as the pixels of HASH_B.
  assert_null(dt_pixel_cache_entry_get_data(entry));

  void *data = NULL;
  dt_pixel_cache_entry_t *held = NULL;
  assert_false(dt_dev_pixelpipe_cache_ref_host_entry_by_hash(HASH_B, &data, &held));
  assert_null(data);

  // The pipeline's own exact-hit lookup finds the line but no pixels, and must not take it as a hit.
  assert_true(dt_dev_pixelpipe_cache_ref_entry_by_hash(HASH_B, &data, &held));
  assert_null(data);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, held);

  // A caller owning no device cannot recover them either.
  assert_false(dt_dev_pixelpipe_cache_peek(HASH_B, &data, NULL, -1, NULL));
}

/** A producer that rewrites the host buffer makes it the line's pixels again. */
static void _rewritten_line_publishes_its_host_pixels(void **state __attribute__((unused)))
{
  const dt_pixel_cache_entry_t first
      = _produce(HASH_A, NULL, TRUE, TRUE, DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED);
  const dt_pixel_cache_entry_t second
      = _produce(HASH_C, &first, TRUE, TRUE, DT_DEV_PIXELPIPE_CACHE_WRITABLE_REKEYED);
  assert_int_equal(second.serial, first.serial);

  void *data = NULL;
  dt_pixel_cache_entry_t *held = NULL;
  assert_true(dt_dev_pixelpipe_cache_ref_host_entry_by_hash(HASH_C, &data, &held));
  assert_non_null(data);
  assert_int_equal(((const unsigned char *)data)[0], (int)(HASH_C & 0xff));
  assert_int_equal(((const unsigned char *)data)[LINE_BYTES - 1], (int)(HASH_C & 0xff));
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, held);
}

/** Without rekey reuse -- the run after a module toggle -- the previous output survives. */
static void _without_rekey_the_previous_output_stays(void **state __attribute__((unused)))
{
  const dt_pixel_cache_entry_t first
      = _produce(HASH_A, NULL, TRUE, TRUE, DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED);
  const dt_pixel_cache_entry_t second
      = _produce(HASH_B, &first, FALSE, TRUE, DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED);
  assert_int_not_equal(second.serial, first.serial);

  // Switching back is an exact hit on the untouched line, host pixels included.
  void *data = NULL;
  dt_pixel_cache_entry_t *held = NULL;
  assert_true(dt_dev_pixelpipe_cache_ref_host_entry_by_hash(HASH_A, &data, &held));
  assert_int_equal(((const unsigned char *)data)[0], (int)(HASH_A & 0xff));
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, held);

  // And the pipeline, asking to write HASH_A again, is told to take it as it is: the line comes back
  // already referenced, so nothing can evict it before the pipeline reads it.
  data = NULL;
  held = NULL;
  assert_int_equal(dt_dev_pixelpipe_cache_get_writable(HASH_A, LINE_BYTES, "test line", 0, TRUE, FALSE,
                                                       &second, NULL, &data, &held),
                   DT_DEV_PIXELPIPE_CACHE_WRITABLE_EXACT_HIT);
  assert_null(data);
  assert_non_null(held);
  assert_int_equal(held->serial, first.serial);
  // One reference, the caller's own, and no lock: the line is readable, and nothing else holds it.
  assert_int_equal(dt_atomic_get_int(&held->refcount), 1);
  gboolean unlocked = FALSE;
  if(dt_pthread_rwlock_trywrlock(&held->lock) == 0)
  {
    unlocked = TRUE;
    dt_pthread_rwlock_unlock(&held->lock);
  }
  assert_true(unlocked);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, held);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_rekeyed_line_publishes_no_host_pixels, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_rewritten_line_publishes_its_host_pixels, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_without_rekey_the_previous_output_stays, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
