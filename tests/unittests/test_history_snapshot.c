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

/* The contract between a history snapshot and the writer's copy-on-write gate.
 *
 * dt_dev_pixelpipe_change() resyncs a pipe against a dt_dev_history_snapshot_t with
 * history_mutex RELEASED. That is only sound if two things hold, and this file pins both:
 *
 *   1. the snapshot holds one reference per item, so nothing it walks can be freed under it;
 *   2. dt_dev_history_cow_touch() clones a shared item instead of mutating it in place, so
 *      nothing the snapshot walks can CHANGE under it either -- and the pipe's last-synced
 *      marker, which the GUI thread re-points at the clone, keeps every refcount balanced.
 *
 * A dt_develop_t is built by hand here: the functions under test touch dev->history,
 * dev->history_mutex, dev->pipe and dev->preview_pipe and nothing else, and dt_dev_init()
 * would drag the whole application in for no gain. */

#include "develop/develop.h"
#include "develop/dev_history.h"
#include "develop/pixelpipe_hb.h"
#include "system/atomic.h"
#include "system/dtpthread.h"

#include <glib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as the other tests here, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

typedef struct _fixture_t
{
  dt_develop_t dev;
  dt_dev_pixelpipe_t pipe;
} _fixture_t;

static int _setup(void **state)
{
  _fixture_t *f = calloc(1, sizeof(_fixture_t));
  if(!f) return -1;
  dt_pthread_rwlock_init(&f->dev.history_mutex, NULL);
  dt_pthread_mutex_init(&f->dev.transient_params_mutex, NULL);
  f->dev.pipe = &f->pipe;
  f->dev.preview_pipe = NULL;
  *state = f;
  return 0;
}

static int _teardown(void **state)
{
  _fixture_t *f = *state;
  // Whatever a test left in the pipe's marker and in history is released here, so a test
  // that asserts on refcounts mid-way does not have to also be its own janitor.
  dt_dev_free_history_item(dt_atomic_exch_ptr(&f->pipe.last_history_item, NULL));
  g_list_free_full(f->dev.history, dt_dev_free_history_item);
  f->dev.history = NULL;
  dt_free(f->dev.transient_params.params);
  dt_free(f->dev.transient_params.blend_params);
  dt_pthread_mutex_destroy(&f->dev.transient_params_mutex);
  dt_pthread_rwlock_destroy(&f->dev.history_mutex);
  free(f);
  return 0;
}

static int _refs(const dt_dev_history_item_t *item)
{
  return dt_atomic_get_int(&((dt_dev_history_item_t *)item)->refcount);
}

static dt_dev_history_item_t *_append(dt_develop_t *dev, const uint64_t hash)
{
  dt_dev_history_item_t *item = dt_dev_history_item_create();
  assert_non_null(item);
  item->hash = hash;
  dev->history = g_list_append(dev->history, item);
  item->num = g_list_index(dev->history, item);
  return item;
}

static void _a_snapshot_references_every_item_and_releases_them(void **state)
{
  _fixture_t *f = *state;
  dt_dev_history_item_t *a = _append(&f->dev, 0xA);
  dt_dev_history_item_t *b = _append(&f->dev, 0xB);
  dt_dev_history_item_t *c = _append(&f->dev, 0xC);
  f->dev.history_end = 3;
  dt_dev_set_history_hash(&f->dev, 0xABC);

  dt_dev_history_snapshot_t snap;
  dt_pthread_rwlock_rdlock(&f->dev.history_mutex);
  dt_dev_history_snapshot_take(&f->dev, &snap);
  dt_pthread_rwlock_unlock(&f->dev.history_mutex);

  // Same items, same order, one extra reference each. No copy: the pointers are identical.
  assert_int_equal(g_list_length(snap.items), 3);
  assert_ptr_equal(g_list_nth_data(snap.items, 0), a);
  assert_ptr_equal(g_list_nth_data(snap.items, 1), b);
  assert_ptr_equal(g_list_nth_data(snap.items, 2), c);
  assert_int_equal(_refs(a), 2);
  assert_int_equal(_refs(b), 2);
  assert_int_equal(_refs(c), 2);

  // end and hash travel with the list.
  assert_int_equal(snap.history_end, 3);
  assert_int_equal(snap.history_hash, 0xABC);

  dt_dev_history_snapshot_release(&snap);
  assert_null(snap.items);
  assert_int_equal(_refs(a), 1);
  assert_int_equal(_refs(b), 1);
  assert_int_equal(_refs(c), 1);
}

static void _history_end_is_clamped_to_the_list_at_snapshot_time(void **state)
{
  _fixture_t *f = *state;
  _append(&f->dev, 0x1);
  _append(&f->dev, 0x2);
  f->dev.history_end = 99; // stale, larger than the list

  dt_dev_history_snapshot_t snap;
  dt_pthread_rwlock_rdlock(&f->dev.history_mutex);
  dt_dev_history_snapshot_take(&f->dev, &snap);
  dt_pthread_rwlock_unlock(&f->dev.history_mutex);

  assert_int_equal(snap.history_end, 2);
  dt_dev_history_snapshot_release(&snap);
}

static void _cow_touch_leaves_an_exclusively_owned_item_alone(void **state)
{
  _fixture_t *f = *state;
  dt_dev_history_item_t *item = _append(&f->dev, 0x1);
  assert_int_equal(_refs(item), 1);

  dt_dev_history_item_t *touched = dt_dev_history_cow_touch(&f->dev, item);

  // Nobody else can see it: mutate in place, no clone, no churn.
  assert_ptr_equal(touched, item);
  assert_int_equal(_refs(item), 1);
  assert_ptr_equal(g_list_nth_data(f->dev.history, 0), item);
}

static void _cow_touch_clones_an_item_a_snapshot_is_holding(void **state)
{
  _fixture_t *f = *state;
  dt_dev_history_item_t *original = _append(&f->dev, 0x1);
  original->enabled = TRUE;
  f->dev.history_end = 1;

  dt_dev_history_snapshot_t snap;
  dt_pthread_rwlock_rdlock(&f->dev.history_mutex);
  dt_dev_history_snapshot_take(&f->dev, &snap);
  dt_pthread_rwlock_unlock(&f->dev.history_mutex);
  assert_int_equal(_refs(original), 2);

  dt_dev_history_item_t *clone = dt_dev_history_cow_touch(&f->dev, original);

  // The writer got a fresh object and dev->history now names it ...
  assert_non_null(clone);
  assert_ptr_not_equal(clone, original);
  assert_ptr_equal(g_list_nth_data(f->dev.history, 0), clone);
  assert_int_equal(_refs(clone), 1);
  assert_int_equal(clone->hash, original->hash);
  assert_true(clone->enabled);

  // ... while the snapshot still names the original, which it alone now keeps alive.
  assert_ptr_equal(g_list_nth_data(snap.items, 0), original);
  assert_int_equal(_refs(original), 1);

  // A mutation of the clone is invisible through the snapshot -- the whole point.
  clone->enabled = FALSE;
  clone->hash = 0x2;
  assert_true(((dt_dev_history_item_t *)g_list_nth_data(snap.items, 0))->enabled);
  assert_int_equal(((dt_dev_history_item_t *)g_list_nth_data(snap.items, 0))->hash, 0x1);

  dt_dev_history_snapshot_release(&snap); // drops the original's last reference
}

static void _cow_touch_repoints_the_pipe_marker_and_balances_every_reference(void **state)
{
  _fixture_t *f = *state;
  dt_dev_history_item_t *original = _append(&f->dev, 0x1);
  f->dev.history_end = 1;

  // The pipe recorded this item as its last-synced marker, holding a reference on it, the
  // way _pipe_set_last_history_item() does at the end of a resync.
  dt_dev_history_item_ref(original);
  dt_atomic_set_ptr(&f->pipe.last_history_item, original);
  assert_int_equal(_refs(original), 2); // history + pipe

  dt_dev_history_snapshot_t snap;
  dt_pthread_rwlock_rdlock(&f->dev.history_mutex);
  dt_dev_history_snapshot_take(&f->dev, &snap);
  dt_pthread_rwlock_unlock(&f->dev.history_mutex);
  assert_int_equal(_refs(original), 3); // history + pipe + snapshot

  dt_dev_history_item_t *clone = dt_dev_history_cow_touch(&f->dev, original);
  assert_ptr_not_equal(clone, original);

  // The marker followed the clone, so an in-place top-entry rewrite keeps its bounded resync;
  // the reference the marker held on the original moved with it.
  assert_ptr_equal(dt_atomic_get_ptr(&f->pipe.last_history_item), clone);
  assert_int_equal(_refs(clone), 2);    // history + pipe
  assert_int_equal(_refs(original), 1); // snapshot only

  dt_dev_history_snapshot_release(&snap);

  // Dropping the pipe's marker the way dt_dev_pixelpipe_cleanup() does leaves the clone
  // owned by history alone: nothing leaked, nothing double-released.
  dt_dev_free_history_item(dt_atomic_exch_ptr(&f->pipe.last_history_item, NULL));
  assert_int_equal(_refs(clone), 1);
}

static void _cow_touch_ignores_a_marker_naming_a_different_item(void **state)
{
  _fixture_t *f = *state;
  dt_dev_history_item_t *first = _append(&f->dev, 0x1);
  dt_dev_history_item_t *second = _append(&f->dev, 0x2);
  f->dev.history_end = 2;

  dt_dev_history_item_ref(first);
  dt_atomic_set_ptr(&f->pipe.last_history_item, first);

  dt_dev_history_snapshot_t snap;
  dt_pthread_rwlock_rdlock(&f->dev.history_mutex);
  dt_dev_history_snapshot_take(&f->dev, &snap);
  dt_pthread_rwlock_unlock(&f->dev.history_mutex);

  dt_dev_history_item_t *clone = dt_dev_history_cow_touch(&f->dev, second);
  assert_ptr_not_equal(clone, second);

  // The marker named `first', not the item being cloned: it must not move.
  assert_ptr_equal(dt_atomic_get_ptr(&f->pipe.last_history_item), first);
  assert_int_equal(_refs(first), 3);  // history + pipe + snapshot
  assert_int_equal(_refs(clone), 1);  // history
  assert_int_equal(_refs(second), 1); // snapshot

  dt_dev_history_snapshot_release(&snap);
}

/**
 * @brief A transient publication must be distinguishable from the one before it.
 *
 * This pins the invariant a realtime module's whole feedback loop rests on, and it is pinned
 * here because nothing else can see it. `dt_iop_commit_params()` hands the params it is given
 * to `module->commit_params()` but derives the piece's cache identity from
 * `dt_iop_compute_module_hash()`, which hashes `module->params` instead -- so a transient edit
 * changes the pixels a recompute would produce WITHOUT changing the hash that decides whether
 * to recompute at all. `_sync_focused_in_place()` closes that by folding this serial into
 * `piece->hash`, which only works if the serial actually advances per publication and arrives
 * with the blob it describes.
 *
 * When this broke, a drawn stroke appeared only after the brush was lifted, every unit test
 * passed, three build configurations were clean, and the log said nothing: the pipe reported
 * success 122 times while exact-hitting the cache. The cost of not having this test was a
 * user-visible regression that only a drawing session could catch.
 */
static void _transient_params_serial_advances_with_every_publication(void **state)
{
  _fixture_t *f = *state;
  dt_iop_module_t module = { 0 };
  module.dev = &f->dev;

  uint32_t params = 0x1111u;
  uint32_t out = 0u;
  uint64_t first = 0u;
  uint64_t second = 0u;
  uint64_t repeat = 0u;

  dt_dev_transient_params_set(&module, &params, sizeof(params), NULL, 0);
  assert_true(dt_dev_transient_params_get(&f->dev, &module, &out, sizeof(out), NULL, 0, NULL, &first));
  assert_int_equal(out, 0x1111u);
  assert_true(first != 0u);

  /* A second publication of DIFFERENT params must not collide with the first. */
  params = 0x2222u;
  dt_dev_transient_params_set(&module, &params, sizeof(params), NULL, 0);
  assert_true(dt_dev_transient_params_get(&f->dev, &module, &out, sizeof(out), NULL, 0, NULL, &second));
  assert_int_equal(out, 0x2222u);
  assert_true(second != first);

  /* And a publication of the SAME bytes must still advance it. A realtime stroke can repaint
   * an identical blob -- what makes the frame new is the canvas the module reads, not the
   * params -- so a serial that only moved on a byte change would drop those frames. */
  dt_dev_transient_params_set(&module, &params, sizeof(params), NULL, 0);
  assert_true(dt_dev_transient_params_get(&f->dev, &module, &out, sizeof(out), NULL, 0, NULL, &repeat));
  assert_true(repeat != second);

  /* A module that published nothing gets no serial and no params. */
  dt_iop_module_t other = { 0 };
  other.dev = &f->dev;
  uint64_t none = 12345u;
  assert_false(dt_dev_transient_params_get(&f->dev, &other, &out, sizeof(out), NULL, 0, NULL, &none));
  assert_int_equal(none, 0u);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_a_snapshot_references_every_item_and_releases_them, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_history_end_is_clamped_to_the_list_at_snapshot_time, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_cow_touch_leaves_an_exclusively_owned_item_alone, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_cow_touch_clones_an_item_a_snapshot_is_holding, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_cow_touch_repoints_the_pipe_marker_and_balances_every_reference, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_cow_touch_ignores_a_marker_naming_a_different_item, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_transient_params_serial_advances_with_every_publication, _setup, _teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
